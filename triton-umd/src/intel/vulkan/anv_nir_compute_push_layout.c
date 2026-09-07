/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_nir.h"
#include "nir_builder.h"
#include "compiler/brw/brw_nir.h"
#include "util/mesa-sha1.h"
#include "util/set.h"

struct push_data {
   bool push_ubo_ranges;
   bool needs_wa_18019110168;
   bool needs_dyn_tess_config;
   unsigned app_start, app_end;
   unsigned driver_start, driver_end;
};

static void
adjust_driver_push_values(nir_shader *nir,
                          enum brw_robustness_flags robust_flags,
                          const struct anv_nir_push_layout_info *push_info,
                          struct brw_base_prog_key *prog_key,
                          const struct intel_device_info *devinfo,
                          struct push_data *data)
{
   if (data->push_ubo_ranges && (robust_flags & BRW_ROBUSTNESS_UBO)) {
      /* We can't on-the-fly adjust our push ranges because doing so would
       * mess up the layout in the shader.  When robustBufferAccess is
       * enabled, we push a mask into the shader indicating which pushed
       * registers are valid and we zero out the invalid ones at the top of
       * the shader.
       */
      const uint32_t push_reg_mask_start =
         anv_drv_const_offset(gfx.push_reg_mask[nir->info.stage]);
      const uint32_t push_reg_mask_end =
         push_reg_mask_start +
         anv_drv_const_size(gfx.push_reg_mask[nir->info.stage]);
      data->driver_start = MIN2(data->driver_start, push_reg_mask_start);
      data->driver_end = MAX2(data->driver_end, push_reg_mask_end);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      if (push_info->fragment_dynamic) {
         const uint32_t fs_config_start = anv_drv_const_offset(gfx.fs_config);
         const uint32_t fs_config_end = fs_config_start +
                                        anv_drv_const_size(gfx.fs_config);
         data->driver_start = MIN2(data->driver_start, fs_config_start);
         data->driver_end = MAX2(data->driver_end, fs_config_end);
      }

      if (data->needs_wa_18019110168) {
         const uint32_t fs_per_prim_remap_start =
            anv_drv_const_offset(gfx.fs_per_prim_remap_offset);
         const uint32_t fs_per_prim_remap_end =
            fs_per_prim_remap_start +
            anv_drv_const_size(gfx.fs_per_prim_remap_offset);
         data->driver_start = MIN2(data->driver_start, fs_per_prim_remap_start);
         data->driver_end = MAX2(data->driver_end, fs_per_prim_remap_end);
      }
   }

   data->needs_dyn_tess_config =
      (nir->info.stage == MESA_SHADER_TESS_CTRL &&
       (container_of(prog_key, struct brw_tcs_prog_key, base)->input_vertices == 0 ||
        push_info->separate_tessellation)) ||
      (nir->info.stage == MESA_SHADER_TESS_EVAL &&
       push_info->separate_tessellation);
   if (data->needs_dyn_tess_config) {
      const uint32_t tess_config_start = anv_drv_const_offset(gfx.tess_config);
      const uint32_t tess_config_end = tess_config_start +
                                       anv_drv_const_size(gfx.tess_config);
      data->driver_start = MIN2(data->driver_start, tess_config_start);
      data->driver_end = MAX2(data->driver_end, tess_config_end);
   }
}

static struct push_data
gather_push_data(nir_shader *nir,
                 enum brw_robustness_flags robust_flags,
                 const struct intel_device_info *devinfo,
                 const struct anv_nir_push_layout_info *push_info,
                 struct brw_base_prog_key *prog_key,
                 struct anv_pipeline_bind_map *map,
                 struct set *lowered_ubo_instrs)
{
   bool has_const_ubo = false;
   struct push_data data = {
      .app_start = UINT_MAX, .app_end = 0,
      .driver_start = UINT_MAX, .driver_end = 0,
   };

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_ubo:
               if (brw_nir_ubo_surface_index_is_pushable(intrin->src[0]) &&
                   nir_src_is_const(intrin->src[1]))
                  has_const_ubo = true;
               break;


            case nir_intrinsic_load_push_constant: {
               unsigned base = nir_intrinsic_base(intrin);
               unsigned range = nir_intrinsic_range(intrin);
               data.app_start = MIN2(data.app_start, base);
               data.app_end = MAX2(data.app_end, base + range);
               break;
            }

            case nir_intrinsic_load_push_data_intel: {
               if (lowered_ubo_instrs &&
                   _mesa_set_search(lowered_ubo_instrs, intrin)) {
                  has_const_ubo = true;
                  break;
               }

               unsigned base = nir_intrinsic_base(intrin);
               unsigned range = nir_intrinsic_range(intrin);
               data.driver_start = MIN2(data.driver_start, base);
               data.driver_end = MAX2(data.driver_end, base + range);
               /* We need to retain this information to update the push
                * constant on vkCmdDispatch*().
                */
               if (nir->info.stage == MESA_SHADER_COMPUTE &&
                   base >= anv_drv_const_offset(cs.num_work_groups[0]) &&
                   base < (anv_drv_const_offset(cs.num_work_groups[2]) + 4))
                  map->binding_mask |= ANV_PIPELINE_BIND_MASK_USES_NUM_WORKGROUP;
               break;
            }

            default:
               break;
            }
         }
      }
   }

   data.push_ubo_ranges =
      has_const_ubo && nir->info.stage != MESA_SHADER_COMPUTE &&
      !brw_shader_stage_requires_bindless_resources(nir->info.stage);

   data.needs_wa_18019110168 =
      nir->info.stage == MESA_SHADER_FRAGMENT &&
      brw_nir_fragment_shader_needs_wa_18019110168(
         devinfo, push_info->mesh_dynamic ? INTEL_SOMETIMES : INTEL_NEVER, nir);

   adjust_driver_push_values(nir, robust_flags, push_info,
                             prog_key, devinfo, &data);

   return data;
}

struct lower_to_push_data_intel_state {
   const struct anv_pipeline_bind_map *bind_map;
   const struct anv_pipeline_push_map *push_map;

   struct set *lowered_ubo_instrs;

   /* Amount that should be subtracted to UBOs loads converted to
    * push_data_intel (in lowered_ubo_instrs)
    */
   unsigned reduced_push_ranges;
};

/* Lower internal UBOs, only used for descriptor buffer loads when the offset
 * is dynamic. We need to add the base offset of the descriptor buffer to the
 * offset relative to the descriptor set.
 */
static bool
lower_internal_ubo(nir_builder *b,
                   nir_intrinsic_instr *intrin)
{
   if (!anv_nir_is_internal_ubo(intrin->src[0]))
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_intrinsic_instr *resource = nir_src_as_intrinsic(intrin->src[0]);

   /* Add the descriptor offset from the resource array_index source to the
    * relative offset.
    */
   nir_src_rewrite(&intrin->src[1],
                   nir_iadd(b, resource->src[2].ssa, intrin->src[1].ssa));

   return true;
}

static bool
lower_ubo_to_push_data_intel(nir_builder *b,
                             nir_intrinsic_instr *intrin,
                             void *_data)
{
   if (intrin->intrinsic != nir_intrinsic_load_ubo)
      return false;

   if (!anv_nir_is_promotable_ubo_binding(intrin->src[0]) ||
       !nir_src_is_const(intrin->src[1]) ||
       brw_shader_stage_requires_bindless_resources(b->shader->info.stage))
      return lower_internal_ubo(b, intrin);

   const struct lower_to_push_data_intel_state *state = _data;
   const int block = anv_nir_get_ubo_binding_push_block(intrin->src[0]);
   assert(block < state->push_map->block_count);
   const struct anv_pipeline_binding *binding =
      &state->push_map->block_to_descriptor[block];
   const unsigned byte_offset = nir_src_as_uint(intrin->src[1]);
   const unsigned num_components =
      nir_def_last_component_read(&intrin->def) + 1;
   const int bytes = num_components * (intrin->def.bit_size / 8);

   uint32_t range_offset = 0;
   const struct anv_push_range *push_range = NULL;
   for (uint32_t i = 0; i < 4; i++) {
      if (state->bind_map->push_ranges[i].set == binding->set &&
          state->bind_map->push_ranges[i].index == binding->index &&
          byte_offset >= state->bind_map->push_ranges[i].start * 32 &&
          (byte_offset + bytes) <= (state->bind_map->push_ranges[i].start +
                                    state->bind_map->push_ranges[i].length) * 32) {
         push_range = &state->bind_map->push_ranges[i];
         break;
      } else {
         range_offset += state->bind_map->push_ranges[i].length * 32;
      }
   }

   if (push_range == NULL)
      return lower_internal_ubo(b, intrin);

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def *data = nir_load_push_data_intel(
      b,
      nir_def_last_component_read(&intrin->def) + 1,
      intrin->def.bit_size,
      nir_imm_int(b, 0),
      .base = range_offset + byte_offset - push_range->start * 32,
      .range = nir_intrinsic_range(intrin));
   nir_def_replace(&intrin->def, data);

   _mesa_set_add(state->lowered_ubo_instrs, nir_def_as_intrinsic(data));

   return true;
}

static bool
lower_to_push_data_intel(nir_builder *b,
                         nir_intrinsic_instr *intrin,
                         void *_data)
{
   const struct lower_to_push_data_intel_state *state = _data;
   /* With bindless shaders we load uniforms with SEND messages. All the push
    * constants are located after the RT_DISPATCH_GLOBALS. We just need to add
    * the offset to the address right after RT_DISPATCH_GLOBALS (see
    * brw_nir_lower_rt_intrinsics.c).
    */
   const unsigned base_offset =
      brw_shader_stage_is_bindless(b->shader->info.stage) ?
      0 : state->bind_map->push_ranges[0].start * 32;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_push_data_intel:
      /* For lowered UBOs to push constants, shrink the base by the amount we
       * shrunk the driver push constants.
       */
      if (_mesa_set_search(state->lowered_ubo_instrs, intrin))
         nir_intrinsic_set_base(intrin, nir_intrinsic_base(intrin) - state->reduced_push_ranges);
      else
         nir_intrinsic_set_base(intrin, nir_intrinsic_base(intrin) - base_offset);
      return true;

   case nir_intrinsic_load_push_constant: {
      b->cursor = nir_before_instr(&intrin->instr);
      nir_def *data = nir_load_push_data_intel(
         b,
         intrin->def.num_components,
         intrin->def.bit_size,
         intrin->src[0].ssa,
         .base = nir_intrinsic_base(intrin) - base_offset,
         .range = nir_intrinsic_range(intrin));
      nir_def_replace(&intrin->def, data);
      return true;
   }

   default:
      return false;
   }
}

static struct anv_push_range
compute_final_push_range(const struct intel_device_info *devinfo,
                         const struct push_data *data)
{
   /* Align push_start down to a 32B (for 3DSTATE_CONSTANT) and make it no
    * larger than push_end (no push constants is indicated by push_start =
    * UINT_MAX).
    *
    * If we were to use
    * 3DSTATE_(MESH|TASK)_SHADER_DATA::IndirectDataStartAddress we would need
    * to align things to 64B.
    *
    * SKL PRMs, Volume 2d: Command Reference: Structures,
    * 3DSTATE_CONSTANT::Constant Buffer 0 Read Length:
    *
    *    "This field specifies the length of the constant data to be loaded
    *     from memory in 256-bit units."
    *
    * ATS-M PRMs, Volume 2d: Command Reference: Structures,
    * 3DSTATE_MESH_SHADER_DATA_BODY::Indirect Data Start Address:
    *
    *    "This pointer is relative to the General State Base Address. It is
    *     the 64-byte aligned address of the indirect data."
    *
    * COMPUTE_WALKER::Indirect Data Start Address has the same requirements as
    * 3DSTATE_MESH_SHADER_DATA_BODY::Indirect Data Start Address but the push
    * constant allocation for compute shader is not shared with other stages
    * (unlike all Gfx stages) and so we can bound+align the allocation there
    * (see anv_cmd_buffer_cs_push_constants).
    */
   unsigned push_start = UINT32_MAX;

   if (data->app_end != 0)
      push_start = MIN2(push_start, data->app_start);
   if (data->driver_end != 0)
      push_start = MIN2(push_start, data->driver_start);

   if (push_start == UINT32_MAX) {
      return (struct anv_push_range) {
         .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      };
   }

   push_start = ROUND_DOWN_TO(push_start, 32);

   const unsigned push_size = align(
      MAX2(data->app_end, data->driver_end) - push_start, devinfo->grf_size);

   return (struct anv_push_range) {
      .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      .start = push_start / 32,
      .length = push_size / 32,
   };
}

bool
anv_nir_compute_push_layout(nir_shader *nir,
                            const struct anv_physical_device *pdevice,
                            enum brw_robustness_flags robust_flags,
                            const struct anv_nir_push_layout_info *push_info,
                            struct brw_base_prog_key *prog_key,
                            struct brw_stage_prog_data *prog_data,
                            struct anv_pipeline_bind_map *map,
                            const struct anv_pipeline_push_map *push_map)
{
   const struct brw_compiler *compiler = pdevice->compiler;
   const struct intel_device_info *devinfo = compiler->devinfo;
   memset(map->push_ranges, 0, sizeof(map->push_ranges));

   struct push_data data =
      gather_push_data(nir, robust_flags, devinfo, push_info, prog_key, map, NULL);

   struct anv_push_range push_constant_range =
      compute_final_push_range(devinfo, &data);

   /* When platforms support Mesh and the fragment shader is not fully linked
    * to the previous shader, payload format can change if the preceding
    * shader is mesh or not, this is an issue in particular for PrimitiveID
    * value (in legacy it's delivered as a VUE slot, in mesh it's delivered as
    * in the per-primitive block).
    *
    * Here is the difference in payload format :
    *
    *       Legacy                 Mesh
    * -------------------   -------------------
    * |      ...        |   |      ...        |
    * |-----------------|   |-----------------|
    * |  Constant data  |   |  Constant data  |
    * |-----------------|   |-----------------|
    * | VUE attributes  |   | Per Primive data|
    * -------------------   |-----------------|
    *                       | VUE attributes  |
    *                       -------------------
    *
    * To solve that issue we push an additional dummy push constant buffer in
    * legacy pipelines to align everything. The compiler then adds a SEL
    * instruction to source the PrimitiveID from the right location based on a
    * dynamic bit in fs_config_intel.
    */
   const bool needs_padding_per_primitive =
      data.needs_wa_18019110168 ||
      (push_info->mesh_dynamic &&
       (nir->info.inputs_read & VARYING_BIT_PRIMITIVE_ID));

   unsigned n_push_ranges = 0;
   unsigned total_push_regs = 0;

   if (push_constant_range.length > 0) {
      map->push_ranges[n_push_ranges++] = push_constant_range;
      total_push_regs += push_constant_range.length;
   }

   struct anv_push_range analysis_ranges[4] = {};
   if (data.push_ubo_ranges) {
      anv_nir_analyze_push_constants_ranges(nir, devinfo, push_map,
                                            analysis_ranges);
   }

   const unsigned max_push_buffers = needs_padding_per_primitive ? 3 : 4;
   const unsigned max_push_regs = needs_padding_per_primitive ? 63 : 64;

   for (unsigned i = 0; i < 4; i++) {
      struct anv_push_range *candidate_range = &analysis_ranges[i];
      if (n_push_ranges >= max_push_buffers)
         break;

      if (candidate_range->length + total_push_regs > max_push_regs)
         candidate_range->length = max_push_regs - total_push_regs;

      if (candidate_range->length == 0)
         break;

      map->push_ranges[n_push_ranges++] = *candidate_range;
      total_push_regs += candidate_range->length;
   }

   /* Pass a single-register push constant payload for the PS stage even if
    * empty, since PS invocations with zero push constant cycles have been
    * found to cause hangs with TBIMR enabled. See HSDES #22020184996.
    *
    * XXX - Use workaround infrastructure and final workaround when provided
    *       by hardware team.
    */
   if (n_push_ranges == 0 &&
       nir->info.stage == MESA_SHADER_FRAGMENT &&
       devinfo->needs_null_push_constant_tbimr_workaround) {
      map->push_ranges[n_push_ranges++] = (struct anv_push_range) {
         .set = ANV_DESCRIPTOR_SET_NULL,
         .start = 0,
         .length = 1,
      };
   }

   if (needs_padding_per_primitive) {
      struct anv_push_range push_constant_padding_range = {
         .set = ANV_DESCRIPTOR_SET_PER_PRIM_PADDING,
         .start = 0,
         .length = 1,
      };
      map->push_ranges[n_push_ranges++] = push_constant_padding_range;
   }

   assert(n_push_ranges <= 4);

   struct lower_to_push_data_intel_state lower_state = {
      .bind_map = map,
      .push_map = push_map,
      .lowered_ubo_instrs = _mesa_pointer_set_create(NULL),
   };

   bool progress = nir_shader_intrinsics_pass(
         nir, lower_ubo_to_push_data_intel,
         nir_metadata_control_flow, &lower_state);

   if (progress && nir_opt_dce(nir)) {
      /* Regather the push data */
      data = gather_push_data(nir, robust_flags, devinfo, push_info, prog_key,
                              map, lower_state.lowered_ubo_instrs);

      /* Update the ranges */
      struct anv_push_range shrinked_push_constant_range =
         compute_final_push_range(devinfo, &data);
      assert(shrinked_push_constant_range.length <= push_constant_range.length);

      if (shrinked_push_constant_range.length > 0) {
         map->push_ranges[0] = shrinked_push_constant_range;
      } else if (map->push_ranges[0].set == shrinked_push_constant_range.set) {
         memmove(&map->push_ranges[0], &map->push_ranges[1], 3 * sizeof(map->push_ranges[0]));
         memset(&map->push_ranges[3], 0, sizeof(map->push_ranges[3]));
      }

      lower_state.reduced_push_ranges = 32 *
         (push_constant_range.length - shrinked_push_constant_range.length);
      push_constant_range = shrinked_push_constant_range;
   }

   /* Finally lower the application's push constants & driver' push data */
   progress |= nir_shader_intrinsics_pass(
      nir, lower_to_push_data_intel,
      nir_metadata_control_flow, &lower_state);

   ralloc_free(lower_state.lowered_ubo_instrs);

   /* Do this before calling brw_cs_fill_push_const_info(), it uses the data
    * in prog_data->push_sizes[].
    */
   for (uint32_t i = 0; i < 4; i++) {
      if (map->push_ranges[i].set == ANV_DESCRIPTOR_SET_PER_PRIM_PADDING)
         continue;

      /* We only bother to shader-zero pushed client UBOs */
      if (map->push_ranges[i].length > 0 &&
          map->push_ranges[i].set < MAX_SETS &&
          (robust_flags & BRW_ROBUSTNESS_UBO))
         prog_data->robust_ubo_ranges |= (uint8_t) (1 << i);

      prog_data->push_sizes[i] = map->push_ranges[i].length * 32;
   }

   unsigned push_start = push_constant_range.start * 32;
   if (prog_data->robust_ubo_ranges) {
      const uint32_t push_reg_mask_offset =
         anv_drv_const_offset(gfx.push_reg_mask[nir->info.stage]);
      assert(push_reg_mask_offset >= push_start);
      prog_data->push_reg_mask_param = (push_reg_mask_offset - push_start) / 4;
   }

   switch (nir->info.stage) {
   case MESA_SHADER_TESS_CTRL:
      if (data.needs_dyn_tess_config) {
         struct brw_tcs_prog_data *tcs_prog_data = brw_tcs_prog_data(prog_data);

         const uint32_t tess_config_offset = anv_drv_const_offset(gfx.tess_config);
         assert(tess_config_offset >= push_start);
         tcs_prog_data->tess_config_param = tess_config_offset - push_start;
      }
      break;

   case MESA_SHADER_TESS_EVAL:
      if (push_info->separate_tessellation) {
         struct brw_tes_prog_data *tes_prog_data = brw_tes_prog_data(prog_data);

         const uint32_t tess_config_offset = anv_drv_const_offset(gfx.tess_config);
         assert(tess_config_offset >= push_start);
         tes_prog_data->tess_config_param = tess_config_offset - push_start;
      }
      break;

   case MESA_SHADER_FRAGMENT: {
      struct brw_fs_prog_data *fs_prog_data =
         container_of(prog_data, struct brw_fs_prog_data, base);

      if (push_info->fragment_dynamic) {
         const uint32_t fs_config_offset =
            anv_drv_const_offset(gfx.fs_config);
         assert(fs_config_offset >= push_start);
         fs_prog_data->fs_config_param = fs_config_offset - push_start;
      }
      if (data.needs_wa_18019110168) {
         const uint32_t fs_per_prim_remap_offset =
            anv_drv_const_offset(gfx.fs_per_prim_remap_offset);
         assert(fs_per_prim_remap_offset >= push_start);
         fs_prog_data->per_primitive_remap_param =
            fs_per_prim_remap_offset - push_start;
      }
      break;
   }

   case MESA_SHADER_COMPUTE: {
      const int subgroup_id_index =
         data.driver_end == (anv_drv_const_offset(cs.subgroup_id) +
                             anv_drv_const_size(cs.subgroup_id)) ?
         (anv_drv_const_offset(cs.subgroup_id) - push_start) / 4 : -1;
      struct brw_cs_prog_data *cs_prog_data = brw_cs_prog_data(prog_data);
      brw_cs_fill_push_const_info(devinfo, cs_prog_data, subgroup_id_index);
      break;
   }

   default:
      break;
   }

#if 0
   fprintf(stderr, "stage=%s push ranges:\n", mesa_shader_stage_name(nir->info.stage));
   for (unsigned i = 0; i < ARRAY_SIZE(map->push_ranges); i++)
      fprintf(stderr, "   range%i: %03u-%03u set=%u index=%u\n", i,
              map->push_ranges[i].start,
              map->push_ranges[i].length,
              map->push_ranges[i].set,
              map->push_ranges[i].index);
#endif

   /* Now that we're done computing the push constant portion of the
    * bind map, hash it.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->push_ranges,
                      sizeof(map->push_ranges),
                      map->push_sha1);
   return progress;
}

void
anv_nir_validate_push_layout(const struct anv_physical_device *pdevice,
                             struct brw_stage_prog_data *prog_data,
                             struct anv_pipeline_bind_map *map)
{
#ifndef NDEBUG
   unsigned prog_data_push_size = 0;
   for (unsigned i = 0; i < 4; i++)
      prog_data_push_size += DIV_ROUND_UP(prog_data->push_sizes[i], 32);

   unsigned bind_map_push_size = 0;
   for (unsigned i = 0; i < 4; i++) {
      /* This is dynamic and doesn't count against prog_data->ubo_ranges[] */
      if (map->push_ranges[i].set == ANV_DESCRIPTOR_SET_PER_PRIM_PADDING)
         continue;
      bind_map_push_size += map->push_ranges[i].length;
   }

   /* We could go through everything again but it should be enough to assert
    * that they push the same number of registers.  This should alert us if
    * the back-end compiler decides to re-arrange stuff or shrink a range.
    */
   assert(prog_data_push_size == bind_map_push_size);
#endif
}
