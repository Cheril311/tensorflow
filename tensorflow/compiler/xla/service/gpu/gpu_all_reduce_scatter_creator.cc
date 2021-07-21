/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/gpu_all_reduce_scatter_creator.h"

#include "tensorflow/compiler/xla/service/all_reduce_scatter_utils.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_query.h"

namespace xla {
namespace gpu {

StatusOr<bool> AllReduceScatterCreator::Run(HloModule *module) {
  const HloModuleConfig &config = module->config();
  int64 next_channel_id = hlo_query::NextChannelId(*module);

  bool changed = false;
  for (HloComputation *computation : module->MakeNonfusionComputations()) {
    for (HloInstruction *instruction :
         computation->MakeInstructionPostOrder()) {
      if (instruction->opcode() != HloOpcode::kAllReduce) {
        continue;
      }
      auto *ar = Cast<HloAllReduceInstruction>(instruction);
      auto ar_spec = MatchAllReduceScatter(ar, config.num_partitions(),
                                           config.replica_count(),
                                           /*allow_multiple_split_dims=*/false,
                                           /*allow_intervening_reshape=*/true);
      if (!ar_spec) {
        VLOG(2) << "Cannot match reduce-scatter " << ar->ToString();
        continue;
      }

      HloInstruction *ds = ar_spec->dynamic_slice;

      // Convert to all-reduce scatter. The output shape of the all-reduce
      // scatter will the same as the input shape, except the split dim size is
      // that of the result of the dynamic slice.
      const int64 split_dim = ar_spec->split_dim;
      Shape scatter_shape = ar->shape();
      TF_RET_CHECK(scatter_shape.dimensions(split_dim) % ar_spec->group_size ==
                   0);
      scatter_shape.set_dimensions(
          split_dim, scatter_shape.dimensions(split_dim) / ar_spec->group_size);

      absl::optional<int64> channel_id;
      if (ar->channel_id()) {
        // We cannot reuse the channel_id on all-reduce for all-reduce-scatter.
        channel_id = next_channel_id++;
      }

      HloInstruction *ars =
          computation->AddInstruction(HloInstruction::CreateAllReduceScatter(
              scatter_shape, ar->operands(), ar->to_apply(),
              ar->replica_groups(), ar->constrain_layout(), channel_id,
              ar->use_global_device_ids(), ar_spec->split_dim));

      // If there was an intervening reshape, reshape the non-split dimensions
      // to match that existing reshape. Basically we can just reshape the ars
      // result to the dynamic slice shape.
      HloInstruction *result = ars;
      HloInstruction *reshape = nullptr;
      if (ds->operand(0) != ar) {
        reshape = ds->mutable_operand(0);
        result = computation->AddInstruction(
            HloInstruction::CreateReshape(ds->shape(), result));
      }

      // Note that RemoveInstructionAndUnusedOperands may not always remove the
      // all-reduce operand of the dynamic-slice, so remove all the dead
      // instructions manually.
      TF_RETURN_IF_ERROR(ds->ReplaceAllUsesWith(result));
      TF_RETURN_IF_ERROR(computation->RemoveInstruction(ds));
      if (reshape) {
        TF_RETURN_IF_ERROR(computation->RemoveInstruction(reshape));
      }
      TF_RETURN_IF_ERROR(computation->RemoveInstructionAndUnusedOperands(ar));
      changed = true;
    }
  }

  return changed;
}

}  // namespace gpu
}  // namespace xla