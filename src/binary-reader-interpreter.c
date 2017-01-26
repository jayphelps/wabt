/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "binary-reader-interpreter.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "allocator.h"
#include "binary-reader.h"
#include "interpreter.h"
#include "writer.h"

#define LOG 0

#if LOG
#define LOGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOGF(...) (void)0
#endif

#define CHECK_RESULT(expr) \
  do {                     \
    if (WASM_FAILED(expr)) \
      return WASM_ERROR;   \
  } while (0)

#define CHECK_DEPTH(ctx, depth)                                         \
  do {                                                                  \
    if ((depth) >= (ctx)->label_stack.size) {                           \
      print_error((ctx), "invalid depth: %d (max %" PRIzd ")", (depth), \
                  ((ctx)->label_stack.size));                           \
      return WASM_ERROR;                                                \
    }                                                                   \
  } while (0)

#define CHECK_LOCAL(ctx, local_index)                                       \
  do {                                                                      \
    uint32_t max_local_index =                                              \
        (ctx)->current_func->defined.param_and_local_types.size;            \
    if ((local_index) >= max_local_index) {                                 \
      print_error((ctx), "invalid local_index: %d (max %d)", (local_index), \
                  max_local_index);                                         \
      return WASM_ERROR;                                                    \
    }                                                                       \
  } while (0)

#define CHECK_GLOBAL(ctx, global_index)                                       \
  do {                                                                        \
    uint32_t max_global_index = (ctx)->global_index_mapping.size;             \
    if ((global_index) >= max_global_index) {                                 \
      print_error((ctx), "invalid global_index: %d (max %d)", (global_index), \
                  max_global_index);                                          \
      return WASM_ERROR;                                                      \
    }                                                                         \
  } while (0)

#define RETURN_IF_TOP_TYPE_IS_ANY(ctx) \
  if (top_type_is_any(ctx))            \
  return

#define RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx) \
  if (top_type_is_any(ctx))               \
  return WASM_OK

typedef uint32_t Uint32;
WASM_DEFINE_VECTOR(uint32, Uint32);
WASM_DEFINE_VECTOR(uint32_vector, Uint32Vector);

typedef enum LabelType {
  LABEL_TYPE_FUNC,
  LABEL_TYPE_BLOCK,
  LABEL_TYPE_LOOP,
  LABEL_TYPE_IF,
  LABEL_TYPE_ELSE,
} LabelType;

typedef struct Label {
  LabelType label_type;
  WasmTypeVector sig;
  uint32_t type_stack_limit;
  uint32_t offset; /* branch location in the istream */
  uint32_t fixup_offset;
} Label;
WASM_DEFINE_VECTOR(label, Label);

typedef struct Context {
  WasmAllocator* allocator;
  WasmBinaryReader* reader;
  WasmBinaryErrorHandler* error_handler;
  WasmAllocator* memory_allocator;
  WasmInterpreterEnvironment* env;
  WasmInterpreterModule* module;
  WasmInterpreterFunc* current_func;
  WasmTypeVector type_stack;
  LabelVector label_stack;
  Uint32VectorVector func_fixups;
  Uint32VectorVector depth_fixups;
  uint32_t depth;
  WasmMemoryWriter istream_writer;
  uint32_t istream_offset;
  /* mappings from module index space to env index space; this won't just be a
   * translation, because imported values will be resolved as well */
  Uint32Vector sig_index_mapping;
  Uint32Vector func_index_mapping;
  Uint32Vector global_index_mapping;

  uint32_t num_func_imports;
  uint32_t num_global_imports;

  /* values cached in the Context so they can be shared between callbacks */
  WasmInterpreterTypedValue init_expr_value;
  uint32_t table_offset;
  WasmBool is_host_import;
  WasmInterpreterModule* host_import_module;
  uint32_t import_env_index;
} Context;

static Label* get_label(Context* ctx, uint32_t depth) {
  assert(depth < ctx->label_stack.size);
  return &ctx->label_stack.data[depth];
}

static Label* top_label(Context* ctx) {
  return get_label(ctx, ctx->label_stack.size - 1);
}

static void handle_error(uint32_t offset, const char* message, Context* ctx) {
  if (ctx->error_handler->on_error) {
    ctx->error_handler->on_error(offset, message,
                                 ctx->error_handler->user_data);
  }
}

static void WASM_PRINTF_FORMAT(2, 3)
    print_error(Context* ctx, const char* format, ...) {
  WASM_SNPRINTF_ALLOCA(buffer, length, format);
  handle_error(WASM_INVALID_OFFSET, buffer, ctx);
}

static uint32_t translate_sig_index_to_env(Context* ctx, uint32_t sig_index) {
  assert(sig_index < ctx->sig_index_mapping.size);
  return ctx->sig_index_mapping.data[sig_index];
}

static WasmInterpreterFuncSignature* get_signature_by_env_index(
    Context* ctx,
    uint32_t sig_index) {
  assert(sig_index < ctx->env->sigs.size);
  return &ctx->env->sigs.data[sig_index];
}

static WasmInterpreterFuncSignature* get_signature_by_module_index(
    Context* ctx,
    uint32_t sig_index) {
  return get_signature_by_env_index(ctx,
                                    translate_sig_index_to_env(ctx, sig_index));
}

static uint32_t translate_func_index_to_env(Context* ctx, uint32_t func_index) {
  assert(func_index < ctx->func_index_mapping.size);
  return ctx->func_index_mapping.data[func_index];
}

static uint32_t translate_module_func_index_to_defined(Context* ctx,
                                                       uint32_t func_index) {
  assert(func_index >= ctx->num_func_imports);
  return func_index - ctx->num_func_imports;
}

static WasmInterpreterFunc* get_func_by_env_index(Context* ctx,
                                                  uint32_t func_index) {
  assert(func_index < ctx->env->funcs.size);
  return &ctx->env->funcs.data[func_index];
}

static WasmInterpreterFunc* get_func_by_module_index(Context* ctx,
                                                     uint32_t func_index) {
  return get_func_by_env_index(ctx,
                               translate_func_index_to_env(ctx, func_index));
}

static uint32_t translate_global_index_to_env(Context* ctx,
                                              uint32_t global_index) {
  assert(global_index < ctx->global_index_mapping.size);
  return ctx->global_index_mapping.data[global_index];
}

static WasmInterpreterGlobal* get_global_by_env_index(Context* ctx,
                                                      uint32_t global_index) {
  assert(global_index < ctx->env->globals.size);
  return &ctx->env->globals.data[global_index];
}

static WasmInterpreterGlobal* get_global_by_module_index(
    Context* ctx,
    uint32_t global_index) {
  return get_global_by_env_index(
      ctx, translate_global_index_to_env(ctx, global_index));
}

static WasmType get_global_type_by_module_index(Context* ctx,
                                                uint32_t global_index) {
  return get_global_by_module_index(ctx, global_index)->typed_value.type;
}

static WasmType get_local_type_by_index(WasmInterpreterFunc* func,
                                        uint32_t local_index) {
  assert(!func->is_host);
  assert(local_index < func->defined.param_and_local_types.size);
  return func->defined.param_and_local_types.data[local_index];
}

static uint32_t translate_local_index(Context* ctx, uint32_t local_index) {
  assert(local_index < ctx->type_stack.size);
  return ctx->type_stack.size - local_index;
}

static uint32_t get_istream_offset(Context* ctx) {
  return ctx->istream_offset;
}

static WasmResult emit_data_at(Context* ctx,
                               size_t offset,
                               const void* data,
                               size_t size) {
  return ctx->istream_writer.base.write_data(
      offset, data, size, ctx->istream_writer.base.user_data);
}

static WasmResult emit_data(Context* ctx, const void* data, size_t size) {
  CHECK_RESULT(emit_data_at(ctx, ctx->istream_offset, data, size));
  ctx->istream_offset += size;
  return WASM_OK;
}

static WasmResult emit_opcode(Context* ctx, WasmOpcode opcode) {
  return emit_data(ctx, &opcode, sizeof(uint8_t));
}

static WasmResult emit_i8(Context* ctx, uint8_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i32(Context* ctx, uint32_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i64(Context* ctx, uint64_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i32_at(Context* ctx, uint32_t offset, uint32_t value) {
  return emit_data_at(ctx, offset, &value, sizeof(value));
}

static WasmResult emit_drop_keep(Context* ctx, uint32_t drop, uint8_t keep) {
  assert(drop != UINT32_MAX);
  assert(keep <= 1);
  if (drop > 0) {
    if (drop == 1 && keep == 0) {
      LOGF("%3" PRIzd ": drop\n", ctx->type_stack.size);
      CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP));
    } else {
      LOGF("%3" PRIzd ": drop_keep %u %u\n", ctx->type_stack.size, drop, keep);
      CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP_KEEP));
      CHECK_RESULT(emit_i32(ctx, drop));
      CHECK_RESULT(emit_i8(ctx, keep));
    }
  }
  return WASM_OK;
}

static WasmResult append_fixup(Context* ctx,
                               Uint32VectorVector* fixups_vector,
                               uint32_t index) {
  if (index >= fixups_vector->size)
    wasm_resize_uint32_vector_vector(ctx->allocator, fixups_vector, index + 1);
  Uint32Vector* fixups = &fixups_vector->data[index];
  uint32_t offset = get_istream_offset(ctx);
  wasm_append_uint32_value(ctx->allocator, fixups, &offset);
  return WASM_OK;
}

static WasmResult emit_br_offset(Context* ctx,
                                 uint32_t depth,
                                 uint32_t offset) {
  if (offset == WASM_INVALID_OFFSET)
    CHECK_RESULT(append_fixup(ctx, &ctx->depth_fixups, depth));
  CHECK_RESULT(emit_i32(ctx, offset));
  return WASM_OK;
}

static uint32_t get_label_br_arity(Label* label) {
  return label->label_type != LABEL_TYPE_LOOP ? label->sig.size : 0;
}

static WasmResult emit_br(Context* ctx, uint32_t depth) {
  Label* label = get_label(ctx, depth);
  uint32_t arity = get_label_br_arity(label);
  assert(ctx->type_stack.size >= label->type_stack_limit + arity);
  uint32_t drop_count =
      (ctx->type_stack.size - label->type_stack_limit) - arity;
  CHECK_RESULT(emit_drop_keep(ctx, drop_count, arity));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR));
  CHECK_RESULT(emit_br_offset(ctx, depth, label->offset));
  return WASM_OK;
}

static WasmResult emit_br_table_offset(Context* ctx, uint32_t depth) {
  Label* label = get_label(ctx, depth);
  uint32_t arity = get_label_br_arity(label);
  assert(ctx->type_stack.size >= label->type_stack_limit + arity);
  uint32_t drop_count =
      (ctx->type_stack.size - label->type_stack_limit) - arity;
  CHECK_RESULT(emit_br_offset(ctx, depth, label->offset));
  CHECK_RESULT(emit_i32(ctx, drop_count));
  CHECK_RESULT(emit_i8(ctx, arity));
  return WASM_OK;
}

static WasmResult fixup_top_label(Context* ctx, uint32_t offset) {
  uint32_t top = ctx->label_stack.size - 1;
  if (top >= ctx->depth_fixups.size) {
    /* nothing to fixup */
    return WASM_OK;
  }

  Uint32Vector* fixups = &ctx->depth_fixups.data[top];
  uint32_t i;
  for (i = 0; i < fixups->size; ++i)
    CHECK_RESULT(emit_i32_at(ctx, fixups->data[i], offset));
  /* reduce the size to 0 in case this gets reused. Keep the allocations for
   * later use */
  fixups->size = 0;
  return WASM_OK;
}

static WasmResult emit_func_offset(Context* ctx,
                                   WasmInterpreterFunc* func,
                                   uint32_t func_index) {
  if (func->defined.offset == WASM_INVALID_OFFSET) {
    uint32_t defined_index =
        translate_module_func_index_to_defined(ctx, func_index);
    CHECK_RESULT(append_fixup(ctx, &ctx->func_fixups, defined_index));
  }
  CHECK_RESULT(emit_i32(ctx, func->defined.offset));
  return WASM_OK;
}

static void on_error(WasmBinaryReaderContext* ctx, const char* message) {
  handle_error(ctx->offset, message, ctx->user_data);
}

static WasmResult on_signature_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_resize_uint32_vector(ctx->allocator, &ctx->sig_index_mapping, count);
  uint32_t i;
  for (i = 0; i < count; ++i)
    ctx->sig_index_mapping.data[i] = ctx->env->sigs.size + i;
  wasm_resize_interpreter_func_signature_vector(ctx->allocator, &ctx->env->sigs,
                                                ctx->env->sigs.size + count);
  return WASM_OK;
}

static WasmResult on_signature(uint32_t index,
                               uint32_t param_count,
                               WasmType* param_types,
                               uint32_t result_count,
                               WasmType* result_types,
                               void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFuncSignature* sig = get_signature_by_module_index(ctx, index);

  wasm_reserve_types(ctx->allocator, &sig->param_types, param_count);
  sig->param_types.size = param_count;
  memcpy(sig->param_types.data, param_types, param_count * sizeof(WasmType));

  wasm_reserve_types(ctx->allocator, &sig->result_types, result_count);
  sig->result_types.size = result_count;
  memcpy(sig->result_types.data, result_types, result_count * sizeof(WasmType));
  return WASM_OK;
}

static WasmResult on_import_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_import_array(ctx->allocator,
                                    &ctx->module->defined.imports, count);
  return WASM_OK;
}

static WasmResult on_import(uint32_t index,
                            WasmStringSlice module_name,
                            WasmStringSlice field_name,
                            void* user_data) {
  Context* ctx = user_data;
  assert(index < ctx->module->defined.imports.size);
  WasmInterpreterImport* import = &ctx->module->defined.imports.data[index];
  import->module_name = wasm_dup_string_slice(ctx->allocator, module_name);
  import->field_name = wasm_dup_string_slice(ctx->allocator, field_name);
  int module_index = wasm_find_binding_index_by_name(
      &ctx->env->registered_module_bindings, &import->module_name);
  if (module_index < 0) {
    print_error(ctx, "unknown import module \"" PRIstringslice "\"",
                WASM_PRINTF_STRING_SLICE_ARG(import->module_name));
    return WASM_ERROR;
  }

  assert((size_t)module_index < ctx->env->modules.size);
  WasmInterpreterModule* module = &ctx->env->modules.data[module_index];
  if (module->is_host) {
    /* We don't yet know the kind of a host import module, so just assume it
     * exists for now. We'll fail later (in on_import_* below) if it doesn't
     * exist). */
    ctx->is_host_import = WASM_TRUE;
    ctx->host_import_module = module;
  } else {
    WasmInterpreterExport* export =
        wasm_get_interpreter_export_by_name(module, &import->field_name);
    if (!export) {
      print_error(ctx, "unknown module field \"" PRIstringslice "\"",
                  WASM_PRINTF_STRING_SLICE_ARG(import->field_name));
      return WASM_ERROR;
    }

    import->kind = export->kind;
    ctx->is_host_import = WASM_FALSE;
    ctx->import_env_index = export->index;
  }
  return WASM_OK;
}

static WasmResult check_import_kind(Context* ctx,
                                    WasmInterpreterImport* import,
                                    WasmExternalKind expected_kind) {
  if (import->kind != expected_kind) {
    print_error(ctx, "expected import \"" PRIstringslice "." PRIstringslice
                     "\" to have kind %s, not %s",
                WASM_PRINTF_STRING_SLICE_ARG(import->module_name),
                WASM_PRINTF_STRING_SLICE_ARG(import->field_name),
                wasm_get_kind_name(expected_kind),
                wasm_get_kind_name(import->kind));
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult check_import_limits(Context* ctx,
                                      const WasmLimits* declared_limits,
                                      const WasmLimits* actual_limits) {
  if (actual_limits->initial < declared_limits->initial) {
    print_error(ctx,
                "actual size (%" PRIu64 ") smaller than declared (%" PRIu64 ")",
                actual_limits->initial, declared_limits->initial);
    return WASM_ERROR;
  }

  if (declared_limits->has_max) {
    if (!actual_limits->has_max) {
      print_error(ctx,
                  "max size (unspecified) larger than declared (%" PRIu64 ")",
                  declared_limits->max);
      return WASM_ERROR;
    } else if (actual_limits->max > declared_limits->max) {
      print_error(ctx,
                  "max size (%" PRIu64 ") larger than declared (%" PRIu64 ")",
                  actual_limits->max, declared_limits->max);
      return WASM_ERROR;
    }
  }

  return WASM_OK;
}

static WasmResult append_export(Context* ctx,
                                WasmInterpreterModule* module,
                                WasmExternalKind kind,
                                uint32_t item_index,
                                WasmStringSlice name) {
  if (wasm_find_binding_index_by_name(&module->export_bindings, &name) != -1) {
    print_error(ctx, "duplicate export \"" PRIstringslice "\"",
                WASM_PRINTF_STRING_SLICE_ARG(name));
    return WASM_ERROR;
  }

  WasmInterpreterExport* export =
      wasm_append_interpreter_export(ctx->allocator, &module->exports);
  export->name = wasm_dup_string_slice(ctx->allocator, name);
  export->kind = kind;
  export->index = item_index;

  WasmBinding* binding = wasm_insert_binding(
      ctx->allocator, &module->export_bindings, &export->name);
  binding->index = module->exports.size - 1;
  return WASM_OK;
}

static void on_host_import_print_error(const char* msg, void* user_data) {
  Context* ctx = user_data;
  print_error(ctx, "%s", msg);
}

static WasmPrintErrorCallback make_print_error_callback(Context* ctx) {
  WasmPrintErrorCallback result;
  result.print_error = on_host_import_print_error;
  result.user_data = ctx;
  return result;
}

static WasmResult on_import_func(uint32_t import_index,
                                 uint32_t func_index,
                                 uint32_t sig_index,
                                 void* user_data) {
  Context* ctx = user_data;
  assert(import_index < ctx->module->defined.imports.size);
  WasmInterpreterImport* import =
      &ctx->module->defined.imports.data[import_index];
  assert(sig_index < ctx->env->sigs.size);
  import->func.sig_index = translate_sig_index_to_env(ctx, sig_index);

  uint32_t func_env_index;
  if (ctx->is_host_import) {
    WasmInterpreterFunc* func =
        wasm_append_interpreter_func(ctx->allocator, &ctx->env->funcs);
    func->is_host = WASM_TRUE;
    func->sig_index = import->func.sig_index;
    func->host.module_name = import->module_name;
    func->host.field_name = import->field_name;

    WasmInterpreterHostImportDelegate* host_delegate =
        &ctx->host_import_module->host.import_delegate;
    WasmInterpreterFuncSignature* sig = &ctx->env->sigs.data[func->sig_index];
    CHECK_RESULT(host_delegate->import_func(import, func, sig,
                                            make_print_error_callback(ctx),
                                            host_delegate->user_data));
    assert(func->host.callback);

    func_env_index = ctx->env->funcs.size - 1;
    append_export(ctx, ctx->host_import_module, WASM_EXTERNAL_KIND_FUNC,
                  func_env_index, import->field_name);
  } else {
    CHECK_RESULT(check_import_kind(ctx, import, WASM_EXTERNAL_KIND_FUNC));
    assert(ctx->import_env_index < ctx->env->funcs.size);
    WasmInterpreterFunc* func = &ctx->env->funcs.data[ctx->import_env_index];
    if (!wasm_func_signatures_are_equal(ctx->env, import->func.sig_index,
                                        func->sig_index)) {
      print_error(ctx, "import signature mismatch");
      return WASM_ERROR;
    }

    func_env_index = ctx->import_env_index;
  }
  wasm_append_uint32_value(ctx->allocator, &ctx->func_index_mapping,
                           &func_env_index);
  ctx->num_func_imports++;
  return WASM_OK;
}

static void init_table_func_indexes(Context* ctx, WasmInterpreterTable* table) {
  wasm_new_uint32_array(ctx->allocator, &table->func_indexes,
                        table->limits.initial);
  size_t i;
  for (i = 0; i < table->func_indexes.size; ++i)
    table->func_indexes.data[i] = WASM_INVALID_INDEX;
}

static WasmResult on_import_table(uint32_t import_index,
                                  uint32_t table_index,
                                  WasmType elem_type,
                                  const WasmLimits* elem_limits,
                                  void* user_data) {
  Context* ctx = user_data;
  if (ctx->module->table_index != WASM_INVALID_INDEX) {
    print_error(ctx, "only one table allowed");
    return WASM_ERROR;
  }

  assert(import_index < ctx->module->defined.imports.size);
  WasmInterpreterImport* import =
      &ctx->module->defined.imports.data[import_index];

  if (ctx->is_host_import) {
    WasmInterpreterTable* table =
        wasm_append_interpreter_table(ctx->allocator, &ctx->env->tables);
    table->limits = *elem_limits;
    init_table_func_indexes(ctx, table);

    WasmInterpreterHostImportDelegate* host_delegate =
        &ctx->host_import_module->host.import_delegate;
    CHECK_RESULT(host_delegate->import_table(import, table,
                                             make_print_error_callback(ctx),
                                             host_delegate->user_data));

    CHECK_RESULT(check_import_limits(ctx, elem_limits, &table->limits));

    ctx->module->table_index = ctx->env->tables.size - 1;
    append_export(ctx, ctx->host_import_module, WASM_EXTERNAL_KIND_TABLE,
                  ctx->module->table_index, import->field_name);
  } else {
    CHECK_RESULT(check_import_kind(ctx, import, WASM_EXTERNAL_KIND_TABLE));
    assert(ctx->import_env_index < ctx->env->tables.size);
    WasmInterpreterTable* table = &ctx->env->tables.data[ctx->import_env_index];
    CHECK_RESULT(check_import_limits(ctx, elem_limits, &table->limits));

    import->table.limits = *elem_limits;
    ctx->module->table_index = ctx->import_env_index;
  }
  return WASM_OK;
}

static WasmResult on_import_memory(uint32_t import_index,
                                   uint32_t memory_index,
                                   const WasmLimits* page_limits,
                                   void* user_data) {
  Context* ctx = user_data;
  if (ctx->module->memory_index != WASM_INVALID_INDEX) {
    print_error(ctx, "only one memory allowed");
    return WASM_ERROR;
  }

  assert(import_index < ctx->module->defined.imports.size);
  WasmInterpreterImport* import =
      &ctx->module->defined.imports.data[import_index];

  if (ctx->is_host_import) {
    WasmInterpreterMemory* memory =
        wasm_append_interpreter_memory(ctx->allocator, &ctx->env->memories);
    memory->allocator = ctx->memory_allocator;

    WasmInterpreterHostImportDelegate* host_delegate =
        &ctx->host_import_module->host.import_delegate;
    CHECK_RESULT(host_delegate->import_memory(import, memory,
                                              make_print_error_callback(ctx),
                                              host_delegate->user_data));

    assert(memory->data);
    CHECK_RESULT(check_import_limits(ctx, page_limits, &memory->page_limits));

    ctx->module->memory_index = ctx->env->memories.size - 1;
    append_export(ctx, ctx->host_import_module, WASM_EXTERNAL_KIND_MEMORY,
                  ctx->module->memory_index, import->field_name);
  } else {
    CHECK_RESULT(check_import_kind(ctx, import, WASM_EXTERNAL_KIND_MEMORY));
    assert(ctx->import_env_index < ctx->env->memories.size);
    WasmInterpreterMemory* memory =
        &ctx->env->memories.data[ctx->import_env_index];
    CHECK_RESULT(check_import_limits(ctx, page_limits, &memory->page_limits));

    import->memory.limits = *page_limits;
    ctx->module->memory_index = ctx->import_env_index;
  }
  return WASM_OK;
}

static WasmResult on_import_global(uint32_t import_index,
                                   uint32_t global_index,
                                   WasmType type,
                                   WasmBool mutable,
                                   void* user_data) {
  Context* ctx = user_data;
  assert(import_index < ctx->module->defined.imports.size);
  WasmInterpreterImport* import =
      &ctx->module->defined.imports.data[import_index];

  uint32_t global_env_index = ctx->env->globals.size - 1;
  if (ctx->is_host_import) {
    WasmInterpreterGlobal* global =
        wasm_append_interpreter_global(ctx->allocator, &ctx->env->globals);
    global->typed_value.type = type;
    global->mutable_ = mutable;

    WasmInterpreterHostImportDelegate* host_delegate =
        &ctx->host_import_module->host.import_delegate;
    CHECK_RESULT(host_delegate->import_global(import, global,
                                              make_print_error_callback(ctx),
                                              host_delegate->user_data));

    global_env_index = ctx->env->globals.size - 1;
    append_export(ctx, ctx->host_import_module, WASM_EXTERNAL_KIND_GLOBAL,
                  global_env_index, import->field_name);
  } else {
    CHECK_RESULT(check_import_kind(ctx, import, WASM_EXTERNAL_KIND_GLOBAL));
    // TODO: check type and mutability
    import->global.type = type;
    import->global.mutable_ = mutable;
    global_env_index = ctx->import_env_index;
  }
  wasm_append_uint32_value(ctx->allocator, &ctx->global_index_mapping,
                           &global_env_index);
  ctx->num_global_imports++;
  return WASM_OK;
}

static WasmResult on_function_signatures_count(uint32_t count,
                                               void* user_data) {
  Context* ctx = user_data;
  size_t old_size = ctx->func_index_mapping.size;
  wasm_resize_uint32_vector(ctx->allocator, &ctx->func_index_mapping,
                            old_size + count);
  uint32_t i;
  for (i = 0; i < count; ++i)
    ctx->func_index_mapping.data[old_size + i] = ctx->env->funcs.size + i;
  wasm_resize_interpreter_func_vector(ctx->allocator, &ctx->env->funcs,
                                      ctx->env->funcs.size + count);
  wasm_resize_uint32_vector_vector(ctx->allocator, &ctx->func_fixups, count);
  return WASM_OK;
}

static WasmResult on_function_signature(uint32_t index,
                                        uint32_t sig_index,
                                        void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFunc* func = get_func_by_module_index(ctx, index);
  func->defined.offset = WASM_INVALID_OFFSET;
  func->sig_index = translate_sig_index_to_env(ctx, sig_index);
  return WASM_OK;
}

static WasmResult on_table(uint32_t index,
                           WasmType elem_type,
                           const WasmLimits* elem_limits,
                           void* user_data) {
  Context* ctx = user_data;
  if (ctx->module->table_index != WASM_INVALID_INDEX) {
    print_error(ctx, "only one table allowed");
    return WASM_ERROR;
  }
  WasmInterpreterTable* table =
      wasm_append_interpreter_table(ctx->allocator, &ctx->env->tables);
  table->limits = *elem_limits;
  init_table_func_indexes(ctx, table);
  ctx->module->table_index = ctx->env->tables.size - 1;
  return WASM_OK;
}

static WasmResult on_memory(uint32_t index,
                            const WasmLimits* page_limits,
                            void* user_data) {
  Context* ctx = user_data;
  if (ctx->module->memory_index != WASM_INVALID_INDEX) {
    print_error(ctx, "only one memory allowed");
    return WASM_ERROR;
  }
  WasmInterpreterMemory* memory =
      wasm_append_interpreter_memory(ctx->allocator, &ctx->env->memories);
  memory->allocator = ctx->memory_allocator;
  memory->page_limits = *page_limits;
  memory->byte_size = page_limits->initial * WASM_PAGE_SIZE;
  memory->data = wasm_alloc_zero(ctx->memory_allocator, memory->byte_size,
                                 WASM_DEFAULT_ALIGN);
  ctx->module->memory_index = ctx->env->memories.size - 1;
  return WASM_OK;
}

static WasmResult on_global_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  size_t old_size = ctx->global_index_mapping.size;
  wasm_resize_uint32_vector(ctx->allocator, &ctx->global_index_mapping,
                            old_size + count);
  uint32_t i;
  for (i = 0; i < count; ++i)
    ctx->global_index_mapping.data[old_size + i] = ctx->env->globals.size + i;
  wasm_resize_interpreter_global_vector(ctx->allocator, &ctx->env->globals,
                                        ctx->env->globals.size + count);
  return WASM_OK;
}

static WasmResult begin_global(uint32_t index,
                               WasmType type,
                               WasmBool mutable_,
                               void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterGlobal* global = get_global_by_module_index(ctx, index);
  global->typed_value.type = type;
  global->mutable_ = mutable_;
  return WASM_OK;
}

static WasmResult end_global_init_expr(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterGlobal* global = get_global_by_module_index(ctx, index);
  if (ctx->init_expr_value.type != global->typed_value.type) {
    print_error(ctx, "type mismatch in global, expected %s but got %s.",
                wasm_get_type_name(global->typed_value.type),
                wasm_get_type_name(ctx->init_expr_value.type));
    return WASM_ERROR;
  }
  global->typed_value = ctx->init_expr_value;
  return WASM_OK;
}

static WasmResult on_init_expr_f32_const_expr(uint32_t index,
                                              uint32_t value_bits,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_F32;
  ctx->init_expr_value.value.f32_bits = value_bits;
  return WASM_OK;
}

static WasmResult on_init_expr_f64_const_expr(uint32_t index,
                                              uint64_t value_bits,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_F64;
  ctx->init_expr_value.value.f64_bits = value_bits;
  return WASM_OK;
}

static WasmResult on_init_expr_get_global_expr(uint32_t index,
                                               uint32_t global_index,
                                               void* user_data) {
  Context* ctx = user_data;
  if (global_index >= ctx->num_global_imports) {
    print_error(ctx,
                "initializer expression can only reference an imported global");
    return WASM_ERROR;
  }
  WasmInterpreterGlobal* ref_global =
      get_global_by_module_index(ctx, global_index);
  if (ref_global->mutable_) {
    print_error(ctx,
                "initializer expression cannot reference a mutable global");
    return WASM_ERROR;
  }
  ctx->init_expr_value = ref_global->typed_value;
  return WASM_OK;
}

static WasmResult on_init_expr_i32_const_expr(uint32_t index,
                                              uint32_t value,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_I32;
  ctx->init_expr_value.value.i32 = value;
  return WASM_OK;
}

static WasmResult on_init_expr_i64_const_expr(uint32_t index,
                                              uint64_t value,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_I64;
  ctx->init_expr_value.value.i64 = value;
  return WASM_OK;
}

static WasmResult on_export(uint32_t index,
                            WasmExternalKind kind,
                            uint32_t item_index,
                            WasmStringSlice name,
                            void* user_data) {
  Context* ctx = user_data;
  switch (kind) {
    case WASM_EXTERNAL_KIND_FUNC:
      item_index = translate_func_index_to_env(ctx, item_index);
      break;

    case WASM_EXTERNAL_KIND_TABLE:
      item_index = ctx->module->table_index;
      break;

    case WASM_EXTERNAL_KIND_MEMORY:
      item_index = ctx->module->memory_index;
      break;

    case WASM_EXTERNAL_KIND_GLOBAL: {
      item_index = translate_global_index_to_env(ctx, item_index);
      WasmInterpreterGlobal* global = &ctx->env->globals.data[item_index];
      if (global->mutable_) {
        print_error(ctx, "mutable globals cannot be exported");
        return WASM_ERROR;
      }
      break;
    }

    case WASM_NUM_EXTERNAL_KINDS:
      assert(0);
      break;
  }
  return append_export(ctx, ctx->module, kind, item_index, name);
}

static WasmResult on_start_function(uint32_t func_index, void* user_data) {
  Context* ctx = user_data;
  uint32_t start_func_index = translate_func_index_to_env(ctx, func_index);
  WasmInterpreterFunc* start_func =
      get_func_by_env_index(ctx, start_func_index);
  WasmInterpreterFuncSignature* sig =
      get_signature_by_env_index(ctx, start_func->sig_index);
  if (sig->param_types.size != 0) {
    print_error(ctx, "start function must be nullary");
    return WASM_ERROR;
  }
  if (sig->result_types.size != 0) {
    print_error(ctx, "start function must not return anything");
    return WASM_ERROR;
  }
  ctx->module->defined.start_func_index = start_func_index;
  return WASM_OK;
}

static WasmResult end_elem_segment_init_expr(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  if (ctx->init_expr_value.type != WASM_TYPE_I32) {
    print_error(ctx, "type mismatch in elem segment, expected i32 but got %s",
                wasm_get_type_name(ctx->init_expr_value.type));
    return WASM_ERROR;
  }
  ctx->table_offset = ctx->init_expr_value.value.i32;
  return WASM_OK;
}

static WasmResult on_elem_segment_function_index_check(uint32_t index,
                                                       uint32_t func_index,
                                                       void* user_data) {
  Context* ctx = user_data;
  assert(ctx->module->table_index != WASM_INVALID_INDEX);
  WasmInterpreterTable* table =
      &ctx->env->tables.data[ctx->module->table_index];
  if (ctx->table_offset >= table->func_indexes.size) {
    print_error(ctx,
                "elem segment offset is out of bounds: %u >= max value %" PRIzd,
                ctx->table_offset, table->func_indexes.size);
    return WASM_ERROR;
  }

  uint32_t max_func_index = (ctx)->func_index_mapping.size;
  if (func_index >= max_func_index) {
    print_error(ctx, "invalid func_index: %d (max %d)", func_index,
                max_func_index);
    return WASM_ERROR;
  }

  table->func_indexes.data[ctx->table_offset++] =
      translate_func_index_to_env(ctx, func_index);
  return WASM_OK;
}

static WasmResult on_elem_segment_function_index(uint32_t index,
                                                 uint32_t func_index,
                                                 void* user_data) {
  Context* ctx = user_data;
  assert(ctx->module->table_index != WASM_INVALID_INDEX);
  WasmInterpreterTable* table =
      &ctx->env->tables.data[ctx->module->table_index];
  table->func_indexes.data[ctx->table_offset++] =
      translate_func_index_to_env(ctx, func_index);
  return WASM_OK;
}

static WasmResult on_data_segment_data_check(uint32_t index,
                                             const void* src_data,
                                             uint32_t size,
                                             void* user_data) {
  Context* ctx = user_data;
  assert(ctx->module->memory_index != WASM_INVALID_INDEX);
  WasmInterpreterMemory* memory =
      &ctx->env->memories.data[ctx->module->memory_index];
  if (ctx->init_expr_value.type != WASM_TYPE_I32) {
    print_error(ctx, "type mismatch in data segment, expected i32 but got %s",
                wasm_get_type_name(ctx->init_expr_value.type));
    return WASM_ERROR;
  }
  uint32_t address = ctx->init_expr_value.value.i32;
  uint64_t end_address = (uint64_t)address + (uint64_t)size;
  if (end_address > memory->byte_size) {
    print_error(ctx, "data segment is out of bounds: [%u, %" PRIu64
                     ") >= max value %u",
                address, end_address, memory->byte_size);
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult on_data_segment_data(uint32_t index,
                                       const void* src_data,
                                       uint32_t size,
                                       void* user_data) {
  Context* ctx = user_data;
  assert(ctx->module->memory_index != WASM_INVALID_INDEX);
  WasmInterpreterMemory* memory =
      &ctx->env->memories.data[ctx->module->memory_index];
  uint32_t address = ctx->init_expr_value.value.i32;
  uint8_t* dst_data = memory->data;
  memcpy(&dst_data[address], src_data, size);
  return WASM_OK;
}

static uint32_t translate_depth(Context* ctx, uint32_t depth) {
  assert(depth < ctx->label_stack.size);
  return ctx->label_stack.size - 1 - depth;
}

static void push_label(Context* ctx,
                       LabelType label_type,
                       const WasmTypeVector* sig,
                       uint32_t offset,
                       uint32_t fixup_offset) {
  Label* label = wasm_append_label(ctx->allocator, &ctx->label_stack);
  label->label_type = label_type;
  wasm_extend_types(ctx->allocator, &label->sig, sig);
  label->type_stack_limit = ctx->type_stack.size;
  label->offset = offset;
  label->fixup_offset = fixup_offset;
  LOGF("   : +depth %" PRIzd "\n", ctx->label_stack.size - 1);
}

static void wasm_destroy_label(WasmAllocator* allocator, Label* label) {
  wasm_destroy_type_vector(allocator, &label->sig);
}

static void pop_label(Context* ctx) {
  LOGF("   : -depth %" PRIzd "\n", ctx->label_stack.size - 1);
  Label* label = top_label(ctx);
  wasm_destroy_label(ctx->allocator, label);
  ctx->label_stack.size--;
  /* reduce the depth_fixups stack as well, but it may be smaller than
   * label_stack so only do it conditionally. */
  if (ctx->depth_fixups.size > ctx->label_stack.size) {
    uint32_t from = ctx->label_stack.size;
    uint32_t to = ctx->depth_fixups.size;
    uint32_t i;
    for (i = from; i < to; ++i)
      wasm_destroy_uint32_vector(ctx->allocator, &ctx->depth_fixups.data[i]);
    ctx->depth_fixups.size = ctx->label_stack.size;
  }
}

static WasmType top_type(Context* ctx) {
  Label* label = top_label(ctx);
  WASM_USE(label);
  assert(ctx->type_stack.size > label->type_stack_limit);
  return ctx->type_stack.data[ctx->type_stack.size - 1];
}

static WasmBool top_type_is_any(Context* ctx) {
  if (ctx->type_stack.size >
      ctx->current_func->defined.param_and_local_types.size) {
    WasmType top_type = ctx->type_stack.data[ctx->type_stack.size - 1];
    if (top_type == WASM_TYPE_ANY)
      return WASM_TRUE;
  }
  return WASM_FALSE;
}

/* TODO(binji): share a lot of the type-checking code w/ wasm-ast-checker.c */
/* TODO(binji): flip actual + expected types, to match wasm-ast-checker.c */
static size_t type_stack_limit(Context* ctx) {
  return top_label(ctx)->type_stack_limit;
}

static WasmResult check_type_stack_limit(Context* ctx,
                                         size_t expected,
                                         const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  size_t limit = type_stack_limit(ctx);
  size_t avail = ctx->type_stack.size - limit;
  if (expected > avail) {
    print_error(ctx, "type stack size too small at %s. got %" PRIzd
                     ", expected at least %" PRIzd,
                desc, avail, expected);
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult check_type_stack_limit_exact(Context* ctx,
                                               size_t expected,
                                               const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  size_t limit = type_stack_limit(ctx);
  size_t avail = ctx->type_stack.size - limit;
  if (expected != avail) {
    print_error(ctx, "type stack at end of %s is %" PRIzd ". expected %" PRIzd,
                desc, avail, expected);
    return WASM_ERROR;
  }
  return WASM_OK;
}

static void reset_type_stack_to_limit(Context* ctx) {
  ctx->type_stack.size = type_stack_limit(ctx);
}

static WasmResult check_type(Context* ctx,
                             WasmType expected,
                             WasmType actual,
                             const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  if (expected != actual) {
    print_error(ctx, "type mismatch in %s, expected %s but got %s.", desc,
                wasm_get_type_name(expected), wasm_get_type_name(actual));
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult check_n_types(Context* ctx,
                                const WasmTypeVector* expected,
                                const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  CHECK_RESULT(check_type_stack_limit(ctx, expected->size, desc));
  /* check the top of the type stack, with values pushed in reverse, against
   * the expected type list; for example, if:
   *   expected = [i32, f32, i32, f64]
   * then
   *   type_stack must be [ ..., f64, i32, f32, i32] */
  size_t i;
  for (i = 0; i < expected->size; ++i) {
    WasmType actual =
        ctx->type_stack.data[ctx->type_stack.size - expected->size + i];
    CHECK_RESULT(
        check_type(ctx, expected->data[expected->size - i - 1], actual, desc));
  }
  return WASM_OK;
}

static WasmType pop_type(Context* ctx) {
  WasmType type = top_type(ctx);
  if (type != WASM_TYPE_ANY) {
    LOGF("%3" PRIzd "->%3" PRIzd ": pop  %s\n", ctx->type_stack.size,
         ctx->type_stack.size - 1, wasm_get_type_name(type));
    ctx->type_stack.size--;
  }
  return type;
}

static void push_type(Context* ctx, WasmType type) {
  RETURN_IF_TOP_TYPE_IS_ANY(ctx);
  if (type != WASM_TYPE_VOID) {
    LOGF("%3" PRIzd "->%3" PRIzd ": push %s\n", ctx->type_stack.size,
         ctx->type_stack.size + 1, wasm_get_type_name(type));
    wasm_append_type_value(ctx->allocator, &ctx->type_stack, &type);
  }
}

static void push_types(Context* ctx, const WasmTypeVector* types) {
  RETURN_IF_TOP_TYPE_IS_ANY(ctx);
  size_t i;
  for (i = 0; i < types->size; ++i)
    push_type(ctx, types->data[i]);
}

static WasmResult pop_and_check_1_type(Context* ctx,
                                       WasmType expected,
                                       const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  if (WASM_SUCCEEDED(check_type_stack_limit(ctx, 1, desc))) {
    WasmType actual = pop_type(ctx);
    CHECK_RESULT(check_type(ctx, expected, actual, desc));
    return WASM_OK;
  }
  return WASM_ERROR;
}

static WasmResult pop_and_check_2_types(Context* ctx,
                                        WasmType expected1,
                                        WasmType expected2,
                                        const char* desc) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  if (WASM_SUCCEEDED(check_type_stack_limit(ctx, 2, desc))) {
    WasmType actual2 = pop_type(ctx);
    WasmType actual1 = pop_type(ctx);
    CHECK_RESULT(check_type(ctx, expected1, actual1, desc));
    CHECK_RESULT(check_type(ctx, expected2, actual2, desc));
    return WASM_OK;
  }
  return WASM_ERROR;
}

static WasmResult check_opcode1(Context* ctx, WasmOpcode opcode) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  CHECK_RESULT(pop_and_check_1_type(ctx, wasm_get_opcode_param_type_1(opcode),
                                    wasm_get_opcode_name(opcode)));
  push_type(ctx, wasm_get_opcode_result_type(opcode));
  return WASM_OK;
}

static WasmResult check_opcode2(Context* ctx, WasmOpcode opcode) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  CHECK_RESULT(pop_and_check_2_types(ctx, wasm_get_opcode_param_type_1(opcode),
                                     wasm_get_opcode_param_type_2(opcode),
                                     wasm_get_opcode_name(opcode)));
  push_type(ctx, wasm_get_opcode_result_type(opcode));
  return WASM_OK;
}

static WasmResult drop_types_for_return(Context* ctx, uint32_t arity) {
  RETURN_OK_IF_TOP_TYPE_IS_ANY(ctx);
  /* drop the locals and params, but keep the return value, if any.  */
  if (ctx->type_stack.size >= arity) {
    uint32_t drop_count = ctx->type_stack.size - arity;
    CHECK_RESULT(emit_drop_keep(ctx, drop_count, arity));
  } else {
    /* it is possible for the size of the type stack to be smaller than the
     * return arity if the last instruction of the function is
     * return. In that case the type stack should be empty */
    assert(ctx->type_stack.size == 0);
  }
  return WASM_OK;
}

static WasmResult begin_function_body(WasmBinaryReaderContext* context,
                                      uint32_t index) {
  Context* ctx = context->user_data;
  WasmInterpreterFunc* func = get_func_by_module_index(ctx, index);
  WasmInterpreterFuncSignature* sig =
      get_signature_by_env_index(ctx, func->sig_index);

  func->is_host = WASM_FALSE;
  func->defined.offset = get_istream_offset(ctx);
  func->defined.local_decl_count = 0;
  func->defined.local_count = 0;

  ctx->current_func = func;
  ctx->depth_fixups.size = 0;
  ctx->type_stack.size = 0;
  ctx->label_stack.size = 0;
  ctx->depth = 0;

  /* fixup function references */
  uint32_t defined_index = translate_module_func_index_to_defined(ctx, index);
  uint32_t i;
  Uint32Vector* fixups = &ctx->func_fixups.data[defined_index];
  for (i = 0; i < fixups->size; ++i)
    CHECK_RESULT(emit_i32_at(ctx, fixups->data[i], func->defined.offset));

  /* append param types */
  for (i = 0; i < sig->param_types.size; ++i) {
    WasmType type = sig->param_types.data[i];
    wasm_append_type_value(ctx->allocator, &func->defined.param_and_local_types,
                           &type);
    wasm_append_type_value(ctx->allocator, &ctx->type_stack, &type);
  }

  /* push implicit func label (equivalent to return) */
  push_label(ctx, LABEL_TYPE_FUNC, &sig->result_types, WASM_INVALID_OFFSET,
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult end_function_body(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  Label* label = top_label(ctx);
  if (!label || label->label_type != LABEL_TYPE_FUNC) {
    print_error(ctx, "unexpected function end");
    return WASM_ERROR;
  }

  CHECK_RESULT(check_n_types(ctx, &label->sig, "implicit return"));
  CHECK_RESULT(check_type_stack_limit_exact(ctx, label->sig.size, "func"));
  fixup_top_label(ctx, get_istream_offset(ctx));
  if (top_type_is_any(ctx)) {
    /* if the top type is any it means that this code is unreachable, at least
     * from the normal fallthrough, though it's possible that this code was
     * reached by branching to the implicit function label. If so, we have
     * already validated that the stack at that location, so we just need to
     * reset it to that state. */
    reset_type_stack_to_limit(ctx);
    push_types(ctx, &label->sig);
  }
  CHECK_RESULT(drop_types_for_return(ctx, label->sig.size));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_RETURN));
  pop_label(ctx);
  ctx->current_func = NULL;
  ctx->type_stack.size = 0;
  return WASM_OK;
}

static WasmResult on_local_decl_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFunc* func = ctx->current_func;
  func->defined.local_decl_count = count;
  return WASM_OK;
}

static WasmResult on_local_decl(uint32_t decl_index,
                                uint32_t count,
                                WasmType type,
                                void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": alloca\n", ctx->type_stack.size);
  WasmInterpreterFunc* func = ctx->current_func;
  func->defined.local_count += count;

  uint32_t i;
  for (i = 0; i < count; ++i) {
    wasm_append_type_value(ctx->allocator, &func->defined.param_and_local_types,
                           &type);
    push_type(ctx, type);
  }

  if (decl_index == func->defined.local_decl_count - 1) {
    /* last local declaration, allocate space for all locals. */
    CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_ALLOCA));
    CHECK_RESULT(emit_i32(ctx, func->defined.local_count));
    /* fixup the function label's type_stack_limit to include these values. */
    Label* label = top_label(ctx);
    assert(label->label_type == LABEL_TYPE_FUNC);
    label->type_stack_limit += func->defined.local_count;
  }
  return WASM_OK;
}

static WasmResult check_has_memory(Context* ctx, WasmOpcode opcode) {
  if (ctx->module->memory_index == WASM_INVALID_INDEX) {
    print_error(ctx, "%s requires an imported or defined memory.",
                wasm_get_opcode_name(opcode));
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult check_align(Context* ctx,
                              uint32_t alignment_log2,
                              uint32_t natural_alignment) {
  if (alignment_log2 >= 32 || (1U << alignment_log2) > natural_alignment) {
    print_error(ctx, "alignment must not be larger than natural alignment (%u)",
                natural_alignment);
    return WASM_ERROR;
  }
  return WASM_OK;
}

static WasmResult on_unary_expr(WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_opcode1(ctx, opcode));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  return WASM_OK;
}

static WasmResult on_binary_expr(WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_opcode2(ctx, opcode));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  return WASM_OK;
}

static WasmResult on_block_expr(uint32_t num_types,
                                WasmType* sig_types,
                                void* user_data) {
  Context* ctx = user_data;
  WasmTypeVector sig;
  sig.size = num_types;
  sig.data = sig_types;
  push_label(ctx, LABEL_TYPE_BLOCK, &sig, WASM_INVALID_OFFSET,
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult on_loop_expr(uint32_t num_types,
                               WasmType* sig_types,
                               void* user_data) {
  Context* ctx = user_data;
  WasmTypeVector sig;
  sig.size = num_types;
  sig.data = sig_types;
  push_label(ctx, LABEL_TYPE_LOOP, &sig, get_istream_offset(ctx),
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult on_if_expr(uint32_t num_types,
                             WasmType* sig_types,
                             void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_type_stack_limit(ctx, 1, "if"));
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "if"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_UNLESS));
  uint32_t fixup_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));

  WasmTypeVector sig;
  sig.size = num_types;
  sig.data = sig_types;
  push_label(ctx, LABEL_TYPE_IF, &sig, WASM_INVALID_OFFSET, fixup_offset);
  return WASM_OK;
}

static WasmResult on_else_expr(void* user_data) {
  Context* ctx = user_data;
  Label* label = top_label(ctx);
  if (!label || label->label_type != LABEL_TYPE_IF) {
    print_error(ctx, "unexpected else operator");
    return WASM_ERROR;
  }

  CHECK_RESULT(check_n_types(ctx, &label->sig, "if true branch"));

  label->label_type = LABEL_TYPE_ELSE;
  uint32_t fixup_cond_offset = label->fixup_offset;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR));
  label->fixup_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  CHECK_RESULT(emit_i32_at(ctx, fixup_cond_offset, get_istream_offset(ctx)));
  /* reset the type stack for the other branch arm */
  ctx->type_stack.size = label->type_stack_limit;
  return WASM_OK;
}

static WasmResult on_end_expr(void* user_data) {
  Context* ctx = user_data;
  Label* label = top_label(ctx);
  if (!label) {
    print_error(ctx, "unexpected end operator");
    return WASM_ERROR;
  }

  const char* desc = NULL;
  switch (label->label_type) {
    case LABEL_TYPE_IF:
    case LABEL_TYPE_ELSE:
      desc = (label->label_type == LABEL_TYPE_IF) ? "if true branch"
                                                  : "if false branch";
      CHECK_RESULT(
          emit_i32_at(ctx, label->fixup_offset, get_istream_offset(ctx)));
      break;

    case LABEL_TYPE_BLOCK:
      desc = "block";
      break;

    case LABEL_TYPE_LOOP:
      desc = "loop";
      break;

    case LABEL_TYPE_FUNC:
      print_error(ctx, "unexpected end operator");
      return WASM_ERROR;
  }

  CHECK_RESULT(check_n_types(ctx, &label->sig, desc));
  CHECK_RESULT(check_type_stack_limit_exact(ctx, label->sig.size, desc));
  fixup_top_label(ctx, get_istream_offset(ctx));
  reset_type_stack_to_limit(ctx);
  push_types(ctx, &label->sig);
  pop_label(ctx);
  return WASM_OK;
}

static WasmResult on_br_expr(uint32_t depth, void* user_data) {
  Context* ctx = user_data;
  CHECK_DEPTH(ctx, depth);
  depth = translate_depth(ctx, depth);
  Label* label = get_label(ctx, depth);
  if (label->label_type != LABEL_TYPE_LOOP)
    CHECK_RESULT(check_n_types(ctx, &label->sig, "br"));
  CHECK_RESULT(emit_br(ctx, depth));
  reset_type_stack_to_limit(ctx);
  push_type(ctx, WASM_TYPE_ANY);
  return WASM_OK;
}

static WasmResult on_br_if_expr(uint32_t depth, void* user_data) {
  Context* ctx = user_data;
  CHECK_DEPTH(ctx, depth);
  depth = translate_depth(ctx, depth);
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "br_if"));
  Label* label = get_label(ctx, depth);
  if (label->label_type != LABEL_TYPE_LOOP)
    CHECK_RESULT(check_n_types(ctx, &label->sig, "br_if"));
  /* flip the br_if so if <cond> is true it can drop values from the stack */
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_UNLESS));
  uint32_t fixup_br_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  CHECK_RESULT(emit_br(ctx, depth));
  CHECK_RESULT(emit_i32_at(ctx, fixup_br_offset, get_istream_offset(ctx)));
  return WASM_OK;
}

static WasmResult on_br_table_expr(WasmBinaryReaderContext* context,
                                   uint32_t num_targets,
                                   uint32_t* target_depths,
                                   uint32_t default_target_depth) {
  Context* ctx = context->user_data;
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "br_table"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_TABLE));
  CHECK_RESULT(emit_i32(ctx, num_targets));
  uint32_t fixup_table_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  /* not necessary for the interpreter, but it makes it easier to disassemble.
   * This opcode specifies how many bytes of data follow. */
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DATA));
  CHECK_RESULT(emit_i32(ctx, (num_targets + 1) * WASM_TABLE_ENTRY_SIZE));
  CHECK_RESULT(emit_i32_at(ctx, fixup_table_offset, get_istream_offset(ctx)));

  uint32_t i;
  for (i = 0; i <= num_targets; ++i) {
    uint32_t depth = i != num_targets ? target_depths[i] : default_target_depth;
    CHECK_DEPTH(ctx, depth);
    depth = translate_depth(ctx, depth);
    Label* label = get_label(ctx, depth);
    CHECK_RESULT(check_n_types(ctx, &label->sig, "br_table"));
    CHECK_RESULT(emit_br_table_offset(ctx, depth));
  }

  reset_type_stack_to_limit(ctx);
  push_type(ctx, WASM_TYPE_ANY);
  return WASM_OK;
}

static WasmResult on_call_expr(uint32_t func_index, void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFunc* func = get_func_by_module_index(ctx, func_index);
  WasmInterpreterFuncSignature* sig =
      get_signature_by_env_index(ctx, func->sig_index);
  CHECK_RESULT(check_type_stack_limit(ctx, sig->param_types.size, "call"));

  uint32_t i;
  for (i = sig->param_types.size; i > 0; --i) {
    WasmType arg = pop_type(ctx);
    CHECK_RESULT(check_type(ctx, sig->param_types.data[i - 1], arg, "call"));
  }

  if (func->is_host) {
    CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL_HOST));
    CHECK_RESULT(emit_i32(ctx, translate_func_index_to_env(ctx, func_index)));
  } else {
    CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL));
    CHECK_RESULT(emit_func_offset(ctx, func, func_index));
  }
  push_types(ctx, &sig->result_types);

  return WASM_OK;
}

static WasmResult on_call_indirect_expr(uint32_t sig_index, void* user_data) {
  Context* ctx = user_data;
  if (ctx->module->table_index == WASM_INVALID_INDEX) {
    print_error(ctx, "found call_indirect operator, but no table");
    return WASM_ERROR;
  }
  WasmInterpreterFuncSignature* sig =
      get_signature_by_module_index(ctx, sig_index);
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "call_indirect"));
  CHECK_RESULT(
      check_type_stack_limit(ctx, sig->param_types.size, "call_indirect"));

  uint32_t i;
  for (i = sig->param_types.size; i > 0; --i) {
    WasmType arg = pop_type(ctx);
    CHECK_RESULT(
        check_type(ctx, sig->param_types.data[i - 1], arg, "call_indirect"));
  }

  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL_INDIRECT));
  CHECK_RESULT(emit_i32(ctx, ctx->module->table_index));
  CHECK_RESULT(emit_i32(ctx, translate_sig_index_to_env(ctx, sig_index)));
  push_types(ctx, &sig->result_types);
  return WASM_OK;
}

static WasmResult on_drop_expr(void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_type_stack_limit(ctx, 1, "drop"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP));
  pop_type(ctx);
  return WASM_OK;
}

static WasmResult on_i32_const_expr(uint32_t value, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_I32_CONST));
  CHECK_RESULT(emit_i32(ctx, value));
  push_type(ctx, WASM_TYPE_I32);
  return WASM_OK;
}

static WasmResult on_i64_const_expr(uint64_t value, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_I64_CONST));
  CHECK_RESULT(emit_i64(ctx, value));
  push_type(ctx, WASM_TYPE_I64);
  return WASM_OK;
}

static WasmResult on_f32_const_expr(uint32_t value_bits, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_F32_CONST));
  CHECK_RESULT(emit_i32(ctx, value_bits));
  push_type(ctx, WASM_TYPE_F32);
  return WASM_OK;
}

static WasmResult on_f64_const_expr(uint64_t value_bits, void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_F64_CONST));
  CHECK_RESULT(emit_i64(ctx, value_bits));
  push_type(ctx, WASM_TYPE_F64);
  return WASM_OK;
}

static WasmResult on_get_global_expr(uint32_t global_index, void* user_data) {
  Context* ctx = user_data;
  CHECK_GLOBAL(ctx, global_index);
  WasmType type = get_global_type_by_module_index(ctx, global_index);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GET_GLOBAL));
  CHECK_RESULT(emit_i32(ctx, translate_global_index_to_env(ctx, global_index)));
  push_type(ctx, type);
  return WASM_OK;
}

static WasmResult on_set_global_expr(uint32_t global_index, void* user_data) {
  Context* ctx = user_data;
  CHECK_GLOBAL(ctx, global_index);
  WasmInterpreterGlobal* global = get_global_by_module_index(ctx, global_index);
  if (global->mutable_ != WASM_TRUE) {
    print_error(ctx, "can't set_global on immutable global at index %u.",
                global_index);
    return WASM_ERROR;
  }
  CHECK_RESULT(
      pop_and_check_1_type(ctx, global->typed_value.type, "set_global"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SET_GLOBAL));
  CHECK_RESULT(emit_i32(ctx, translate_global_index_to_env(ctx, global_index)));
  return WASM_OK;
}

static WasmResult on_get_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_type_by_index(ctx->current_func, local_index);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GET_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  push_type(ctx, type);
  return WASM_OK;
}

static WasmResult on_set_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_type_by_index(ctx->current_func, local_index);
  CHECK_RESULT(pop_and_check_1_type(ctx, type, "set_local"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SET_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  return WASM_OK;
}

static WasmResult on_tee_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_type_by_index(ctx->current_func, local_index);
  CHECK_RESULT(check_type_stack_limit(ctx, 1, "tee_local"));
  WasmType value = top_type(ctx);
  CHECK_RESULT(check_type(ctx, type, value, "tee_local"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_TEE_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  return WASM_OK;
}

static WasmResult on_grow_memory_expr(void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_has_memory(ctx, WASM_OPCODE_GROW_MEMORY));
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "grow_memory"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GROW_MEMORY));
  CHECK_RESULT(emit_i32(ctx, ctx->module->memory_index));
  push_type(ctx, WASM_TYPE_I32);
  return WASM_OK;
}

static WasmResult on_load_expr(WasmOpcode opcode,
                               uint32_t alignment_log2,
                               uint32_t offset,
                               void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_has_memory(ctx, opcode));
  CHECK_RESULT(
      check_align(ctx, alignment_log2, wasm_get_opcode_memory_size(opcode)));
  CHECK_RESULT(check_opcode1(ctx, opcode));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  CHECK_RESULT(emit_i32(ctx, ctx->module->memory_index));
  CHECK_RESULT(emit_i32(ctx, offset));
  return WASM_OK;
}

static WasmResult on_store_expr(WasmOpcode opcode,
                                uint32_t alignment_log2,
                                uint32_t offset,
                                void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_has_memory(ctx, opcode));
  CHECK_RESULT(
      check_align(ctx, alignment_log2, wasm_get_opcode_memory_size(opcode)));
  CHECK_RESULT(check_opcode2(ctx, opcode));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  CHECK_RESULT(emit_i32(ctx, ctx->module->memory_index));
  CHECK_RESULT(emit_i32(ctx, offset));
  return WASM_OK;
}

static WasmResult on_current_memory_expr(void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(check_has_memory(ctx, WASM_OPCODE_CURRENT_MEMORY));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CURRENT_MEMORY));
  CHECK_RESULT(emit_i32(ctx, ctx->module->memory_index));
  push_type(ctx, WASM_TYPE_I32);
  return WASM_OK;
}

static WasmResult on_nop_expr(void* user_data) {
  return WASM_OK;
}

static WasmResult on_return_expr(void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFuncSignature* sig =
      get_signature_by_env_index(ctx, ctx->current_func->sig_index);
  CHECK_RESULT(check_n_types(ctx, &sig->result_types, "return"));
  CHECK_RESULT(drop_types_for_return(ctx, sig->result_types.size));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_RETURN));
  reset_type_stack_to_limit(ctx);
  push_type(ctx, WASM_TYPE_ANY);
  return WASM_OK;
}

static WasmResult on_select_expr(void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(pop_and_check_1_type(ctx, WASM_TYPE_I32, "select"));
  CHECK_RESULT(check_type_stack_limit(ctx, 2, "select"));
  WasmType right = pop_type(ctx);
  WasmType left = pop_type(ctx);
  CHECK_RESULT(check_type(ctx, left, right, "select"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SELECT));
  push_type(ctx, left);
  return WASM_OK;
}

static WasmResult on_unreachable_expr(void* user_data) {
  Context* ctx = user_data;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_UNREACHABLE));
  reset_type_stack_to_limit(ctx);
  push_type(ctx, WASM_TYPE_ANY);
  return WASM_OK;
}

static WasmBinaryReader s_binary_reader = {
    .user_data = NULL,
    .on_error = on_error,

    .on_signature_count = on_signature_count,
    .on_signature = on_signature,

    .on_import_count = on_import_count,
    .on_import = on_import,
    .on_import_func = on_import_func,
    .on_import_table = on_import_table,
    .on_import_memory = on_import_memory,
    .on_import_global = on_import_global,

    .on_function_signatures_count = on_function_signatures_count,
    .on_function_signature = on_function_signature,

    .on_table = on_table,

    .on_memory = on_memory,

    .on_global_count = on_global_count,
    .begin_global = begin_global,
    .end_global_init_expr = end_global_init_expr,

    .on_export = on_export,

    .on_start_function = on_start_function,

    .begin_function_body = begin_function_body,
    .on_local_decl_count = on_local_decl_count,
    .on_local_decl = on_local_decl,
    .on_binary_expr = on_binary_expr,
    .on_block_expr = on_block_expr,
    .on_br_expr = on_br_expr,
    .on_br_if_expr = on_br_if_expr,
    .on_br_table_expr = on_br_table_expr,
    .on_call_expr = on_call_expr,
    .on_call_indirect_expr = on_call_indirect_expr,
    .on_compare_expr = on_binary_expr,
    .on_convert_expr = on_unary_expr,
    .on_current_memory_expr = on_current_memory_expr,
    .on_drop_expr = on_drop_expr,
    .on_else_expr = on_else_expr,
    .on_end_expr = on_end_expr,
    .on_f32_const_expr = on_f32_const_expr,
    .on_f64_const_expr = on_f64_const_expr,
    .on_get_global_expr = on_get_global_expr,
    .on_get_local_expr = on_get_local_expr,
    .on_grow_memory_expr = on_grow_memory_expr,
    .on_i32_const_expr = on_i32_const_expr,
    .on_i64_const_expr = on_i64_const_expr,
    .on_if_expr = on_if_expr,
    .on_load_expr = on_load_expr,
    .on_loop_expr = on_loop_expr,
    .on_nop_expr = on_nop_expr,
    .on_return_expr = on_return_expr,
    .on_select_expr = on_select_expr,
    .on_set_global_expr = on_set_global_expr,
    .on_set_local_expr = on_set_local_expr,
    .on_store_expr = on_store_expr,
    .on_tee_local_expr = on_tee_local_expr,
    .on_unary_expr = on_unary_expr,
    .on_unreachable_expr = on_unreachable_expr,
    .end_function_body = end_function_body,

    .end_elem_segment_init_expr = end_elem_segment_init_expr,
    .on_elem_segment_function_index = on_elem_segment_function_index_check,

    .on_data_segment_data = on_data_segment_data_check,

    .on_init_expr_f32_const_expr = on_init_expr_f32_const_expr,
    .on_init_expr_f64_const_expr = on_init_expr_f64_const_expr,
    .on_init_expr_get_global_expr = on_init_expr_get_global_expr,
    .on_init_expr_i32_const_expr = on_init_expr_i32_const_expr,
    .on_init_expr_i64_const_expr = on_init_expr_i64_const_expr,
};

/* Second pass to assign data and elem segments after they are checked above. */
static WasmBinaryReader s_binary_reader_segments = {
    .user_data = NULL,
    .on_error = on_error,

    .end_elem_segment_init_expr = end_elem_segment_init_expr,
    .on_elem_segment_function_index = on_elem_segment_function_index,
    .on_data_segment_data = on_data_segment_data,

    .on_init_expr_f32_const_expr = on_init_expr_f32_const_expr,
    .on_init_expr_f64_const_expr = on_init_expr_f64_const_expr,
    .on_init_expr_get_global_expr = on_init_expr_get_global_expr,
    .on_init_expr_i32_const_expr = on_init_expr_i32_const_expr,
    .on_init_expr_i64_const_expr = on_init_expr_i64_const_expr,
};

static void destroy_context(Context* ctx) {
  wasm_destroy_type_vector(ctx->allocator, &ctx->type_stack);
  WASM_DESTROY_VECTOR_AND_ELEMENTS(ctx->allocator, ctx->label_stack, label);
  WASM_DESTROY_VECTOR_AND_ELEMENTS(ctx->allocator, ctx->depth_fixups,
                                   uint32_vector);
  WASM_DESTROY_VECTOR_AND_ELEMENTS(ctx->allocator, ctx->func_fixups,
                                   uint32_vector);
  wasm_destroy_uint32_vector(ctx->allocator, &ctx->sig_index_mapping);
  wasm_destroy_uint32_vector(ctx->allocator, &ctx->func_index_mapping);
  wasm_destroy_uint32_vector(ctx->allocator, &ctx->global_index_mapping);
}

WasmResult wasm_read_binary_interpreter(WasmAllocator* allocator,
                                        WasmAllocator* memory_allocator,
                                        WasmInterpreterEnvironment* env,
                                        const void* data,
                                        size_t size,
                                        const WasmReadBinaryOptions* options,
                                        WasmBinaryErrorHandler* error_handler,
                                        WasmInterpreterModule** out_module) {
  Context ctx;
  WasmBinaryReader reader;

  WasmInterpreterEnvironmentMark mark = wasm_mark_interpreter_environment(env);

  WasmInterpreterModule* module =
      wasm_append_interpreter_module(allocator, &env->modules);

  WASM_ZERO_MEMORY(ctx);
  WASM_ZERO_MEMORY(reader);

  ctx.allocator = allocator;
  ctx.reader = &reader;
  ctx.error_handler = error_handler;
  ctx.memory_allocator = memory_allocator;
  ctx.env = env;
  ctx.module = module;
  ctx.module->is_host = WASM_FALSE;
  ctx.module->table_index = WASM_INVALID_INDEX;
  ctx.module->memory_index = WASM_INVALID_INDEX;
  ctx.module->defined.start_func_index = WASM_INVALID_INDEX;
  ctx.module->defined.istream_start = env->istream.size;
  ctx.istream_offset = env->istream.size;
  CHECK_RESULT(
      wasm_init_mem_writer_existing(&ctx.istream_writer, &env->istream));

  reader = s_binary_reader;
  reader.user_data = &ctx;

  const uint32_t num_function_passes = 1;
  WasmResult result = wasm_read_binary(allocator, data, size, &reader,
                                       num_function_passes, options);
  wasm_steal_mem_writer_output_buffer(&ctx.istream_writer, &env->istream);
  if (WASM_SUCCEEDED(result)) {
    /* Another pass on the read binary to assign data and elem segments. */
    reader = s_binary_reader_segments;
    reader.user_data = &ctx;
    result = wasm_read_binary(allocator, data, size, &reader,
                              num_function_passes, options);
    assert(WASM_SUCCEEDED(result));

    env->istream.size = ctx.istream_offset;
    ctx.module->defined.istream_end = env->istream.size;
    *out_module = module;
  } else {
    wasm_reset_interpreter_environment_to_mark(allocator, env, mark);
    *out_module = NULL;
  }
  destroy_context(&ctx);
  return result;
}
