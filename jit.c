#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <janet.h>

#define ENABLE_DATAFLOW_TYPESPECIALIZATION 1

typedef struct {
  size_t count;
  size_t capacity;
  Janet *argv;
} CallArgs;

typedef Janet (*JitFn)(int32_t argc, Janet *argv, CallArgs *call_args);

// jit helpers called form emitted assembly
uint64_t jit_typecheck(Janet val, uint32_t types) {
  if (!janet_checktypes(val, types)) {
    janet_panicf("expected %T, got %v", types, val);
  }
}

uint64_t jit_nil(Janet val) {
  return janet_checktype(val, JANET_NIL);
}

uint64_t jit_equals(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_equals(lhs, rhs)));
}

uint64_t jit_not_equals(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(!janet_equals(lhs, rhs)));
}

uint64_t jit_in(Janet collection, Janet key) {
  // TODO: this one can maybe get a assembly fast path
  return janet_u64(janet_in(collection, key));
}

// TODO: rename this to poiner/value comparable
uint64_t jit_u64_orderable(Janet lhs, Janet rhs) {
    // more types are possible to compare without falling back, but require their own special cases
  return (janet_checktype(lhs, JANET_NUMBER)) &&
    (janet_checktype(rhs, JANET_NUMBER));
}

uint64_t jit_call(Janet callee, CallArgs *call_args) {
  uint64_t result;
  if (janet_checktype(callee, JANET_FUNCTION)) {
    JanetFunction *func = janet_unwrap_function(callee);
    result = janet_u64(janet_call(func, call_args->count, call_args->argv));
  } else if (janet_checktype(callee, JANET_CFUNCTION)) {
    JanetCFunction func = janet_unwrap_cfunction(callee);
    result = janet_u64(func(call_args->count, call_args->argv));
  } else if (janet_checktype(callee, JANET_ABSTRACT)) {
    JanetAbstract abstract = janet_unwrap_abstract(callee);
    const JanetAbstractType *at = janet_abstract_type(abstract);
    if (at->call != NULL) {
      result = janet_u64(at->call(abstract, call_args->count, call_args->argv));
    } else {
      janet_panic("attempted to call uncallable abstract type");
    }
  } else if (janet_checktype(callee, JANET_KEYWORD)) {
    if (call_args->count == 0) {
      janet_panic("keyword argument on nil value");
    }

    Janet kwcallee = janet_get(call_args->argv[0], callee);
    if (janet_checktype(kwcallee, JANET_FUNCTION)) {
      JanetFunction *func = janet_unwrap_function(kwcallee);
      result = janet_u64(janet_call(func, call_args->count, call_args->argv));
    } else {
      janet_panicf("keyword function %p, %p is not callable", callee, kwcallee);
    }
  } else {
    janet_panic("attempted to call uncallable type");
  }
  call_args->count = 0;
  return result;
}

uint64_t jit_make_array(CallArgs *call_args) {
  Janet a = janet_wrap_array(janet_array_n(call_args->argv, call_args->count));
  call_args->count = 0;
  return janet_u64(a);
}

uint64_t jit_make_tuple(CallArgs *call_args) {
  JanetTuple t = janet_tuple_n(call_args->argv, call_args->count);
  Janet tup = janet_wrap_tuple(t);
  call_args->count = 0;
  return janet_u64(tup);
}

uint64_t jit_make_bracket_tuple(CallArgs *call_args) {
  JanetTuple t = janet_tuple_n(call_args->argv, call_args->count);
  janet_tuple_flag(t) |= JANET_TUPLE_FLAG_BRACKETCTOR;
  Janet tup = janet_wrap_tuple(t);
  call_args->count = 0;
  return janet_u64(tup);
}

uint64_t jit_make_buffer(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer(call_args->count * 10);
  for (int i = 0; i < call_args->count; i++) {
    janet_to_string_b(b, call_args->argv[i]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_buffer(b));
}

uint64_t jit_make_string(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer(call_args->count * 10);
  for (int i = 0; i < call_args->count; i++) {
    janet_to_string_b(b, call_args->argv[i]);
  }
  call_args->count = 0;
  // TODO: this leaves a garbage buffer we can potentially skip
  return janet_u64(janet_stringv(b->data, b->count));
}

uint64_t jit_make_table(CallArgs *call_args) {
  if (call_args->count & 1) {
    janet_panicf("expected even number of arguments to table constructor, got %d", call_args->count);
  }
  JanetTable *tab = janet_table(call_args->count / 2);
  for (int i = 0; i < call_args->count; i += 2) {
    janet_table_put(tab, call_args->argv[i], call_args->argv[i + 1]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_table(tab));
}

uint64_t jit_make_struct(CallArgs *call_args) {
  if (call_args->count & 1) {
    janet_panicf("expected even number of arguments to struct constructor, got %d", call_args->count);
  }
  JanetKV *st = janet_struct_begin(call_args->count / 2);
  for (int i = 0; i < call_args->count; i += 2) {
    janet_struct_put(st, call_args->argv[i], call_args->argv[i + 1]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_struct(janet_struct_end(st)));
}

void ensure_argv_space(CallArgs *call_args, size_t request) {
  if (call_args->capacity == 0) {
    call_args->capacity = (request > 8) ? request : 8;
    call_args->argv = malloc(call_args->capacity * sizeof(Janet));
  } else if (call_args->count + request > call_args->capacity) {
    call_args->capacity *= 2;
    call_args->argv = realloc(call_args->argv, call_args->capacity * sizeof(Janet));
  }
}

void jit_push(Janet value, CallArgs *call_args) {
  ensure_argv_space(call_args, 1);
  call_args->argv[call_args->count++] = value;
}

void jit_push_2(Janet value1, Janet value2, CallArgs *call_args) {
  ensure_argv_space(call_args, 2);
  call_args->argv[call_args->count++] = value1;
  call_args->argv[call_args->count++] = value2;
}

void jit_push_3(Janet value1, Janet value2, Janet value3, CallArgs *call_args) {
  ensure_argv_space(call_args, 3);
  call_args->argv[call_args->count++] = value1;
  call_args->argv[call_args->count++] = value2;
  call_args->argv[call_args->count++] = value3;
}

#define JIT_UNKNOWN (JANET_POINTER + 1)
#define JIT_OTHER (JANET_POINTER + 1)

typedef struct {
  JanetType t;
  JanetType result; // used for functions
} JitFlowInfo;

typedef struct {
  JanetCFunction cfun;
  JanetType result;
} CFunTypeInfo;

typedef struct {
  JanetFunction *fun;
  JanetType result;
} FunTypeInfo;


static int cfun_info_count = 0;
static CFunTypeInfo cfun_info[256];

static int fun_info_count = 0;
static FunTypeInfo fun_info[256];

typedef struct {
  void *code;
  size_t code_size;
  JanetFunction *fallback;
  JitFlowInfo *flow;
  size_t signature_argc;
  JanetType *signature_arg_types;
} JittedFunction;

typedef struct {
  uint8_t *data;
  size_t count;
  size_t capacity;
  int *jump_targets;
  int *jump_locations;
  int jump_index;
} CodeBuffer;

void print_specialized_bytecode(JittedFunction *jitted) {
  JanetFunction *fn = jitted->fallback;
  JanetFuncDef *def = fn->def;
  size_t bc_len = def->bytecode_length;
  int32_t def_slots = def->slotcount;
  JitFlowInfo *flow = jitted->flow;

  for (int i = 0; i < bc_len; i++) {
    uint32_t instr = def->bytecode[i];
    Janet instruction = janet_asm_decode_instruction(instr);

    int32_t len;
    const Janet *elements;
    if (!janet_indexed_view(instruction, &elements, &len)) {
      janet_panic("unable to view instruction");
    }

    printf("%i. %s | ", i, janet_unwrap_symbol(elements[0]));
    for (int j = 0; j < def_slots; j++) {
      if (flow[(i * def_slots) + j].t == JIT_UNKNOWN) {
	printf("  _____  |");
      } else if (flow[(i * def_slots) + j].t == JIT_OTHER) {
	printf("  .....  |");
      } else {
	printf("  %s  |", janet_type_names[flow[(i * def_slots) + j].t]);
      }
    }
    printf("\n");
  }
}

// TODO: use information from typecheck calls
void dataflow(JittedFunction *jitted, int32_t argc, Janet *argv) {
  JanetFunction *fn = jitted->fallback;
  JanetFuncDef *def = fn->def;
  size_t bc_len = def->bytecode_length;
  int32_t def_slots = def->slotcount;

  // all slot types at instruction time
  JitFlowInfo *slot_types = malloc(bc_len * def_slots * sizeof(JitFlowInfo));
  for (int i = 0; i < bc_len * def_slots; i++) {
    slot_types[i].t = JIT_UNKNOWN;
  }

  for (int i = 0; i < argc; i++) {
    slot_types[i].t = janet_type(argv[i]);
  }

  int did_jump = 0;

  for (int i = 0; i < bc_len; i++) {
    uint32_t instr = def->bytecode[i];
    int opcode = instr & 0xFF;
    int a = (instr >> 8) & 0xFF;
    int b = (instr >> 16) & 0xFF;
    int c = (instr >> 24) & 0xFF;
    int32_t imm = (int32_t)instr >> 16;
    int32_t imm8 = (int32_t)instr >> 24;
    uint32_t d = (uint32_t)instr >> 8;
    uint32_t e = (uint32_t)instr >> 16;

    if (i > 0 && did_jump == 0) {
      // TODO: use memcpy
      for (int j = 0; j < def_slots; j++) {
	slot_types[(i * def_slots) + j] = slot_types[((i - 1) * def_slots) + j];
      }
    } else {
      did_jump = 0;
    }

    switch (opcode) {
    case JOP_ADD_IMMEDIATE:
    case JOP_ADD:
    case JOP_SUBTRACT_IMMEDIATE:
    case JOP_SUBTRACT:
    case JOP_MULTIPLY_IMMEDIATE:
    case JOP_MULTIPLY:
    case JOP_DIVIDE_IMMEDIATE:
    case JOP_DIVIDE:
    case JOP_DIVIDE_FLOOR:
    case JOP_MODULO:
    case JOP_REMAINDER:
    case JOP_BAND:
    case JOP_BOR:
    case JOP_BXOR:
    case JOP_BNOT:
    case JOP_SHIFT_LEFT:
    case JOP_SHIFT_LEFT_IMMEDIATE:
    case JOP_SHIFT_RIGHT:
    case JOP_SHIFT_RIGHT_IMMEDIATE:
    case JOP_SHIFT_RIGHT_UNSIGNED:
    case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE:
    case JOP_LENGTH:
      // This is a shortcut we are taking and isn't interpreter accurate
      slot_types[(i * def_slots) + a].t = JANET_NUMBER;
      break;
    case JOP_MOVE_FAR:
      slot_types[(i * def_slots) + e].t = slot_types[(i * def_slots) + a].t;
      break;
    case JOP_MOVE_NEAR:
      slot_types[(i * def_slots) + a].t = slot_types[(i * def_slots) + e].t;
      break;
    case JOP_LOAD_NIL:
      slot_types[(i * def_slots) + a].t = JANET_NIL;
      break;
    case JOP_LOAD_TRUE:
      slot_types[(i * def_slots) + a].t = JANET_BOOLEAN;
      break;
    case JOP_LOAD_FALSE:
      slot_types[(i * def_slots) + a].t = JANET_BOOLEAN;
      break;
    case JOP_LOAD_INTEGER:
      slot_types[(i * def_slots) + a].t = JANET_NUMBER;
      break;
    case JOP_LOAD_SELF:
      slot_types[(i * def_slots) + a].t = JANET_FUNCTION;
      // TODO: we can figure out our own return types in some cases...
      slot_types[(i * def_slots) + a].result = JIT_UNKNOWN;
      break;
    case JOP_LOAD_CONSTANT: {
      slot_types[(i * def_slots) + a].t = janet_type(def->constants[e]);

      if (slot_types[(i * def_slots) + a].t == JANET_CFUNCTION) {
	slot_types[(i * def_slots) + a].result = JIT_UNKNOWN;
	JanetCFunction cfun = janet_unwrap_cfunction(def->constants[e]);
	// TODO: use cfun_info_count
	for (int j = 0; j < cfun_info_count; j++) {
	  if (cfun_info[j].cfun == cfun) {
	    slot_types[(i * def_slots) + a].result = cfun_info[j].result;
	  }
	}
      }

      if (slot_types[(i * def_slots) + a].t == JANET_FUNCTION) {
	slot_types[(i * def_slots) + a].result = JIT_UNKNOWN;
	JanetFunction *fun = janet_unwrap_function(def->constants[e]);
	// TODO: use cfun_info_count
	for (int j = 0; j < fun_info_count; j++) {
	  if (fun_info[j].fun == fun) {
	    slot_types[(i * def_slots) + a].result = fun_info[j].result;
	  }
	}
      }
      break;
    }
    case JOP_JUMP_IF:
    case JOP_JUMP_IF_NOT:
    case JOP_JUMP_IF_NIL:
    case JOP_JUMP_IF_NOT_NIL: {
      int32_t offset = ((int32_t)instr >> 16);
      for (int j = 0; j < def_slots; j++) {
	slot_types[((i + offset) * def_slots) + j] = slot_types[(i * def_slots) + j];
      }
      break;
    }
    case JOP_RETURN:
      did_jump = 1;
      break;
    case JOP_RETURN_NIL:
      did_jump = 1;
      break;
    case JOP_JUMP: {
      int32_t offset = ((int32_t)instr >> 8);
      for (int j = 0; j < def_slots; j++) {
	slot_types[((i + offset) * def_slots) + j] = slot_types[(i * def_slots) + j];
      }
      did_jump = 1;
      break;
    }
    case JOP_EQUALS:
    case JOP_EQUALS_IMMEDIATE:
    case JOP_NOT_EQUALS:
    case JOP_NOT_EQUALS_IMMEDIATE:
      slot_types[(i * def_slots) + a].t = JANET_BOOLEAN;
      break;
    case JOP_CALL: {
      slot_types[(i * def_slots) + a].t = JIT_OTHER;
      if (slot_types[(i * def_slots) + e].t == JANET_CFUNCTION && slot_types[(i * def_slots) + e].result < JIT_UNKNOWN) {
	slot_types[(i * def_slots) + a].t = slot_types[(i * def_slots) + e].result;
      } else if (slot_types[(i * def_slots) + e].t == JANET_FUNCTION && slot_types[(i * def_slots) + e].result < JIT_UNKNOWN) {
	slot_types[(i * def_slots) + a].t = slot_types[(i * def_slots) + e].result;
      }
      break;
      }
    case JOP_MAKE_BUFFER:
      slot_types[(i * def_slots) + d].t = JANET_BUFFER;
      break;
    case JOP_MAKE_ARRAY:
      slot_types[(i * def_slots) + d].t = JANET_ARRAY;
      break;
    case JOP_MAKE_STRING:
      slot_types[(i * def_slots) + d].t = JANET_STRING;
      break;
    case JOP_MAKE_TUPLE:
      slot_types[(i * def_slots) + d].t = JANET_TUPLE;
      break;
    case JOP_MAKE_TABLE:
      slot_types[(i * def_slots) + d].t = JANET_TABLE;
      break;
    case JOP_MAKE_STRUCT:
      slot_types[(i * def_slots) + d].t = JANET_STRUCT;
      break;
    case JOP_IN:
    case JOP_GET:
    case JOP_GET_INDEX:
      slot_types[(i * def_slots) + a].t = JIT_UNKNOWN;
      break;
    }
  }
  jitted->flow = slot_types;
  /* print_specialized_bytecode(jitted); */
}

static int jitted_function_gc(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  if (jitted->code != NULL) {
    munmap(jitted->code, jitted->code_size);
  }
  if (jitted->signature_arg_types != NULL) {
    free(jitted->signature_arg_types);
  }
  return 0;
}

static int jitted_function_gcmark(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  janet_mark(janet_wrap_function(jitted->fallback));
  return 0;
}
static void emit_byte(CodeBuffer *code, uint8_t byte) {
  if (code->count >= code->capacity) {
    code->capacity *= 2;
    code->data = realloc(code->data, code->capacity);
  }
  code->data[code->count++] = byte;
}

static void emit_u32(CodeBuffer *code, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

static void emit_u64(CodeBuffer *code, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

static void emit_stack_to_gpr(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // xmm(dest), rsi + source
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((dest & 8) ? 0x04 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2C);
  emit_byte(code, 0x84 + ((dest & 7) << 3)); // store here
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet)); // from here
}


static void emit_stack_to_xmm(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // xmm(dest), rsi + source
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x10);
  emit_byte(code, 0x84 + (dest << 3));
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet));
}

static void emit_xmm_to_stack(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // rsi + source, xmm(dest)
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x11);
  emit_byte(code, 0x84 + (source << 3));
  emit_byte(code, 0x24);
  emit_u32(code, dest * sizeof(Janet));
}


static void emit_binary_op(CodeBuffer *code, uint8_t op, uint32_t dest, uint32_t lhs, uint32_t rhs) {
  // op lhs, rhs
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, op); // operation
  emit_byte(code, 0xC0 + (lhs << 3) + rhs);
}

// used for first arg when jumping back to C
static int arg_loc[] = { 7, 6, 2, 1 };
static void emit_stack_to_arg(CodeBuffer *code, uint32_t dest, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84 + (arg_loc[dest] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc * sizeof(Janet));
}

static void emit_non_janet_to_arg(CodeBuffer *code, uint32_t dest, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84 + (arg_loc[dest] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc);
}

static void emit_arg_to_frame(CodeBuffer *code, uint32_t source, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84 + (arg_loc[source] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc);
}


static void emit_cfun_call(CodeBuffer *code, void *func) {
  emit_byte(code, 0x48);
  emit_byte(code, 0xB8);
  emit_u64(code, (uint64_t)(uintptr_t)func);
  emit_byte(code, 0xFF);
  emit_byte(code, 0xD0);
}

static void emit_imm_rax(CodeBuffer *code, uint64_t val) {
  emit_byte(code, 0x48);
  emit_byte(code, 0xB8);
  emit_u64(code, val);
}

// rax -> stack rsp + stack_offset
static void emit_store_ret(CodeBuffer *code, uint32_t offset) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84);
  emit_byte(code, 0x24);
  emit_u32(code, offset * sizeof(Janet));
}

static void compile_bytecode(CodeBuffer *code, JittedFunction *jitted, int pc, uint32_t instr, int stack_size) {
  // TODO: these can be #define/macros, but this is fine for now

  JanetFunction *fn = jitted->fallback;
  Janet *constants = fn->def->constants;
  int call_args_loc = fn->def->slotcount * sizeof(Janet);
  int opcode = instr & 0xFF;
  int a = (instr >> 8) & 0xFF;
  int b = (instr >> 16) & 0xFF;
  int c = (instr >> 24) & 0xFF;
  int32_t imm = (int32_t)instr >> 16;
  int32_t imm8 = (int32_t)instr >> 24;
  uint32_t d = (uint32_t)instr >> 8;
  uint32_t e = (uint32_t)instr >> 16;
  int jump_patch;
  int jump_patch_done;
  int distance;

  switch (opcode) {
  case JOP_NOOP:
    break;
  case JOP_ERROR:
    emit_stack_to_arg(code, 0, a);
    emit_cfun_call(code, janet_panicv);
    break;
  case JOP_TYPECHECK:
    emit_stack_to_arg(code, 0, a);
    // immediate type number
    emit_byte(code, 0xBE);
    emit_u32(code, e);
    emit_cfun_call(code, jit_typecheck);
    break;
  case JOP_RETURN:
    // mov rax, rsp + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));

    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }
    emit_byte(code, 0xC3); // ret
    break;
  case JOP_RETURN_NIL:
    // mov rax, imm(nil)
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_nil()));

    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }
    emit_byte(code, 0xC3); // ret
    break;
  case JOP_ADD_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x58, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_ADD:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xmm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x58, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SUBTRACT_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SUBTRACT:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xmm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MULTIPLY_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x59, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MULTIPLY:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x59, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x5E, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5E, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE_FLOOR:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5E, a, 0, 1);
    // floor(xmm0)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (0 << 3) + 0);
    emit_byte(code, 0x01); // <- round instead of trunc
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MODULO:
    // a - truncate(a / b) * b
    // a -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // a -> xmm1
    emit_stack_to_xmm(code, 1, b);
    // b -> xmm2
    emit_stack_to_xmm(code, 2, c);
    // xmm1 / xmm2
    emit_binary_op(code, 0x5E, a, 1, 2);
    // floor(xmm1)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (1 << 3) + 1);
    emit_byte(code, 0x01); // <- round instead of trunc
    // xmm1 * xmm2
    emit_binary_op(code, 0x59, a, 1, 2);
    // xmm0 - xmm1
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_REMAINDER:
    // a - truncate(a / b) * b
    // a -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // a -> xmm1
    emit_stack_to_xmm(code, 1, b);
    // b -> xmm2
    emit_stack_to_xmm(code, 2, c);
    // xmm1 / xmm2
    emit_binary_op(code, 0x5E, a, 1, 2);
    // trunc(xmm1)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (1 << 3) + 1);
    emit_byte(code, 0x03); // <- trunc instead of round
    // xmm1 * xmm2
    emit_binary_op(code, 0x59, a, 1, 2);
    // xmm0 - xmm1
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BAND:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (band rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x21); // and
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BOR:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (band rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x09); // or
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> rsi + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BXOR:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (bxor rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x31); // xor
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BNOT:
    // e = value
    // value -> RAX
    emit_stack_to_gpr(code, 0, e);
    emit_byte(code, 0x48);
    emit_byte(code, 0xF7);
    emit_byte(code, 0xD0); // NOT

    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> rsi + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_LEFT:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // blshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xE0);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_LEFT_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift imm8
    emit_byte(code, 0xC1);
    emit_byte(code, 0xE0);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // brshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xF8);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift
    emit_byte(code, 0xC1);
    emit_byte(code, 0xF8);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_UNSIGNED:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // brshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xF8);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift
    emit_byte(code, 0xC1);
    emit_byte(code, 0xF8);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MOVE_FAR:
    emit_stack_to_xmm(code, 0, a);
    emit_xmm_to_stack(code, e, 0);
    break;
  case JOP_MOVE_NEAR:
    emit_stack_to_xmm(code, 0, e);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_JUMP:
    emit_byte(code, 0xE9);
    // location to patch in the 32bit destination
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 8);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF:
    // a -> arg 1
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_truthy);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x85); // JNZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NOT:
    emit_stack_to_arg(code, 0, a);
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_truthy);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x84); // JZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NIL:
    emit_stack_to_arg(code, 0, a);
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_nil);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x85); // JNZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NOT_NIL:
    emit_stack_to_arg(code, 0, a);
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_nil);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x84); // JZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_GREATER_THAN:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_GREATER_THAN_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_GREATER_THAN_EQUAL:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x93); // SETAE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_LESS_THAN:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_LESS_THAN_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> xmm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_LESS_THAN_EQUAL:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x96); // SETBE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
    // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_EQUALS:
    if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + b].t == JANET_NUMBER &&
	jitted->flow[(pc * fn->def->slotcount) + c].t == JANET_NUMBER) {
      // numeric fast path
      emit_stack_to_xmm(code, 0, b);
      emit_stack_to_xmm(code, 1, c);
      // ucomisd left, right sets ZF when equal and PF when either value is NaN
      emit_byte(code, 0x66);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2E);
      emit_byte(code, 0xC0 + (0 << 3) + 1);
      // set AL based on result of comparison
      // AL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x94); // SETE
      emit_byte(code, 0xC0); // AL
      // DL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x9B); // SETNP - make nan not equal
      emit_byte(code, 0xC2); // DL
      // (and al dl)
      emit_byte(code, 0x20);
      emit_byte(code, 0xD0);
      // store al
      emit_byte(code, 0x0F);
      emit_byte(code, 0xB6);
      emit_byte(code, 0xD0); // MOVZX EDX, AL.
      // false -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0xB8);
      emit_u64(code, janet_u64(janet_wrap_false()));
      // OR RAX, RDX
      emit_byte(code, 0x48);
      emit_byte(code, 0x09);
      emit_byte(code, 0xD0);
      emit_store_ret(code, a);
    } else if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + b].t == JANET_KEYWORD &&
	jitted->flow[(pc * fn->def->slotcount) + c].t == JANET_KEYWORD) {
      /* printf("emitting keyword fast path\n"); */
      // lhs RDI
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (7 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, b * sizeof(Janet));
      // rhs RSI
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (6 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, c * sizeof(Janet));
      // cmp rdi, rsi
      emit_byte(code, 0x48);
      emit_byte(code, 0x39);
      emit_byte(code, 0xF7);
      // set AL based on result of comparison
      // AL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x94); // SETE
      emit_byte(code, 0xC0); // AL
      // store al
      emit_byte(code, 0x0F);
      emit_byte(code, 0xB6);
      emit_byte(code, 0xD0); // MOVZX EDX, AL.
      // false -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0xB8);
      emit_u64(code, janet_u64(janet_wrap_false()));
      // OR RAX, RDX
      emit_byte(code, 0x48);
      emit_byte(code, 0x09);
      emit_byte(code, 0xD0);
      emit_store_ret(code, a);
    } else {
      emit_stack_to_arg(code, 0, b);
      emit_stack_to_arg(code, 1, c);
      // go back to the interpreter
      emit_cfun_call(code, jit_equals);
      emit_store_ret(code, a);
    }
    break;
  case JOP_EQUALS_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x94); // SETE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_NOT_EQUALS:
    if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + b].t == JANET_NUMBER &&
	jitted->flow[(pc * fn->def->slotcount) + c].t == JANET_NUMBER) {
      /* printf("emitting numeric fast path\n"); */
      emit_stack_to_xmm(code, 0, b);
      emit_stack_to_xmm(code, 1, c);
      // ucomisd left, right sets ZF when equal and PF when either value is NaN
      emit_byte(code, 0x66);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2E);
      emit_byte(code, 0xC0 + (0 << 3) + 1);
      // set AL based on result of comparison
      // AL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x95); // SETNE
      emit_byte(code, 0xC0); // AL
      // DL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x9A); // SETP - make nan not equal
      emit_byte(code, 0xC2); // DL
      // (or al dl)
      emit_byte(code, 0x08);
      emit_byte(code, 0xD0);
      // store al
      emit_byte(code, 0x0F);
      emit_byte(code, 0xB6);
      emit_byte(code, 0xD0); // MOVZX EDX, AL.
      // false -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0xB8);
      emit_u64(code, janet_u64(janet_wrap_false()));
      // OR RAX, RDX
      emit_byte(code, 0x48);
      emit_byte(code, 0x09);
      emit_byte(code, 0xD0);
      // RAX -> stack + offset
      emit_byte(code, 0x48);
      emit_byte(code, 0x89);
      emit_byte(code, 0x84);
      emit_byte(code, 0x24);
      emit_u32(code, a * sizeof(Janet));
    } else if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + b].t == JANET_KEYWORD &&
	jitted->flow[(pc * fn->def->slotcount) + c].t == JANET_KEYWORD) {
      /* printf("emitting keyword fast path\n"); */
      emit_stack_to_arg(code, 0, b);
      emit_stack_to_arg(code, 1, c);
      // cmp rdi, rsi
      emit_byte(code, 0x48);
      emit_byte(code, 0x39);
      emit_byte(code, 0xF7);
      // set AL based on result of comparison
      // AL
      emit_byte(code, 0x0F);
      emit_byte(code, 0x95); // SETNE
      emit_byte(code, 0xC0); // AL
      // store al
      emit_byte(code, 0x0F);
      emit_byte(code, 0xB6);
      emit_byte(code, 0xD0); // MOVZX EDX, AL.
      // false -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0xB8);
      emit_u64(code, janet_u64(janet_wrap_false()));
      // OR RAX, RDX
      emit_byte(code, 0x48);
      emit_byte(code, 0x09);
      emit_byte(code, 0xD0);
      emit_store_ret(code, a);
    } else {
      // lhs RDI
      emit_stack_to_arg(code, 0, b);
      // rhs RSI
      emit_stack_to_arg(code, 1, c);
      emit_cfun_call(code, jit_not_equals);
      emit_store_ret(code, a);
    }
    break;
  case JOP_NOT_EQUALS_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x95); // SETNE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9A); // SETP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (or al dl)
    emit_byte(code, 0x08);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
    // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    emit_store_ret(code, a);
    break;
  case JOP_COMPARE:
    // check numeric for fast path
    // b = lhs c = rhs
    // lhs 7 (first arg)
    emit_stack_to_arg(code, 0, b);
    // rhs 6 (second arg)
    emit_stack_to_arg(code, 1, c);
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_u64_orderable);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // check done
    // test if return (EAX) 1
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // jump to slow path
    emit_byte(code, 0x0F);
    emit_byte(code, 0x84);
    jump_patch = code->count;
    emit_u32(code, 0);

    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // compare
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // seta al
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA -- above
    emit_byte(code, 0xC0); // AL
    // setb dl
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB -- below
    emit_byte(code, 0xC2); // DL
    // setp cl
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9A); // SETP - make nan not equal
    emit_byte(code, 0xC1); // CL
    // movzx, eax, al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xC0);
    // movzx ecx, cl
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xC9);
    // zero extend eax
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD2);
    // sub eax, edx
    emit_byte(code, 0x29);
    emit_byte(code, 0xD0);
    // lea eax, rax + rcx*2
    emit_byte(code, 0x8D);
    emit_byte(code, 0x04);
    emit_byte(code, 0x48);
    // eax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);
    // jump to done
    emit_byte(code, 0xE9);
    jump_patch_done = code->count;
    emit_u32(code, 0);

    // patch the jump
    distance = code->count - (jump_patch + 4);
    for (int i = 0; i < 4; i++) {
      code->data[jump_patch + i] = (distance >> (i * 8));
    }

    // TODO: this can also probably have a fast path
    // b = lhs c = rhs
    // lhs 7 (first arg)
    emit_stack_to_arg(code, 0, b);
    // rhs 6 (second arg)
    emit_stack_to_arg(code, 1, c);
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_compare);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);

    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);

    // patch the jump
    distance = code->count - (jump_patch_done + 4);
    for (int i = 0; i < 4; i++) {
      code->data[jump_patch_done + i] = (distance >> (i * 8));
    }
    break;
  case JOP_LOAD_NIL:
    emit_imm_rax(code, janet_u64(janet_wrap_nil()));
    emit_store_ret(code, d);
    break;
  case JOP_LOAD_TRUE:
    emit_imm_rax(code, janet_u64(janet_wrap_true()));
    emit_store_ret(code, d);
    break;
  case JOP_LOAD_FALSE:
    emit_imm_rax(code, janet_u64(janet_wrap_false()));
    emit_store_ret(code, d);
    break;
  case JOP_LOAD_INTEGER:
    emit_imm_rax(code, janet_u64(janet_wrap_integer(imm)));
    emit_store_ret(code, a);
    break;
  case JOP_LOAD_CONSTANT:
    emit_imm_rax(code, janet_u64(constants[e]));
    emit_store_ret(code, a);
    break;
  case JOP_LOAD_UPVALUE:
    janet_panic("janet's upvalue opcode is not supported");
    break;
  case JOP_LOAD_SELF:
    emit_imm_rax(code, janet_u64(janet_wrap_function(fn)));
    // store rax on stack
    emit_store_ret(code, d);
    break;
  case JOP_SET_UPVALUE:
    janet_panic("janet's set upvalue opcode is not supported");
    break;
  case JOP_CLOSURE:
    janet_panic("janet's closure opcode is not supported");
    break;
  case JOP_PUSH:
    // d = value to push
    // put the error in RDI (first arg)
    emit_stack_to_arg(code, 0, d);
    emit_non_janet_to_arg(code, 1, call_args_loc);
    emit_cfun_call(code, jit_push);
    break;
  case JOP_PUSH_2:
    // a, e
    emit_stack_to_arg(code, 0, a);
    emit_stack_to_arg(code, 1, e);
    emit_non_janet_to_arg(code, 2, call_args_loc);
    emit_cfun_call(code, jit_push_2);
    break;
  case JOP_PUSH_3:
    // a, b, c
    // put the error in RDI (first arg)
    emit_stack_to_arg(code, 0, a);
    emit_stack_to_arg(code, 1, b);
    emit_stack_to_arg(code, 2, c);
    emit_non_janet_to_arg(code, 3, call_args_loc);
    emit_cfun_call(code, jit_push_3);
    break;
    /* case JOP_PUSH_ARRAY: */
  case JOP_CALL:
    // a = dest e = callee
    // put the error in RDI (first arg)
    emit_stack_to_arg(code, 0, e);
    emit_non_janet_to_arg(code, 1, call_args_loc);
    emit_cfun_call(code, jit_call);
    emit_store_ret(code, a);
    break;
  case JOP_TAILCALL:
    // d = callee
    // put the error in RDI (first arg)
    emit_stack_to_arg(code, 0, d);
    emit_non_janet_to_arg(code, 1, call_args_loc);
    emit_cfun_call(code, jit_call);
    emit_store_ret(code, a);
    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }

    emit_byte(code, 0xC3); // ret
    break;
  case JOP_RESUME:
    janet_panic("janet's resume opcode is not supported");
    break;
  case JOP_SIGNAL:
    janet_panic("janet's signal opcode is not supported");
    break;
  case JOP_PROPAGATE:
    janet_panic("janet's propagate opcode is not supported");
    break;
  case JOP_IN:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_stack_to_arg(code, 0, b);
    emit_stack_to_arg(code, 1, c);
    emit_cfun_call(code, janet_in);
    emit_store_ret(code, a);
    break;
  case JOP_GET:
    // b = collection, c = key
    // collection arg 1 (7)
    if (ENABLE_DATAFLOW_TYPESPECIALIZATION &&
	jitted->flow[(pc * fn->def->slotcount) + b].t == JANET_TUPLE
	&& jitted->flow[(pc * fn->def->slotcount) + c].t == JANET_NUMBER) {
      // e = collection
      // e -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (0 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, b * sizeof(Janet));
      // shift off tag bits
      // left 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE0);
      emit_byte(code, 0x11);
      // right 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE8);
      emit_byte(code, 0x11);
      // load key
      emit_stack_to_xmm(code, 0, c);
      // make sure number is int (this rounds and is incorrect in a sense)
      emit_byte(code, 0xF2);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2C);
      emit_byte(code, 0xD0);
      // TODO: bounds check, positive check, exact integer check

      // load tuple[index]
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x04);
      emit_byte(code, 0xD0);

      emit_store_ret(code, a);
    } else {
      emit_stack_to_arg(code, 0, b);
      emit_stack_to_arg(code, 1, c);
      emit_cfun_call(code, janet_get);
      emit_store_ret(code, a);
    }
    break;
  case JOP_GET_INDEX:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_stack_to_arg(code, 0, b);
    // key (6)
    // c immidiate (6)
    emit_byte(code, 0xBE);
    emit_u32(code, c);

    emit_cfun_call(code, janet_getindex);
    emit_store_ret(code, a);
    break;
  case JOP_PUT:
    // a = collection, b = key, c = val
    // collection arg 1 (7)
    emit_stack_to_arg(code, 0, a);
    emit_stack_to_arg(code, 1, b);
    emit_stack_to_arg(code, 2, c);
    emit_cfun_call(code, janet_put);
    break;
  case JOP_PUT_INDEX:
    // a = collection, c = key b = value
    // collection arg 1 (7)
    emit_stack_to_arg(code, 0, a);
    // c immidiate (6)
    emit_byte(code, 0xBE);
    emit_u32(code, c);

    // val (2)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (2 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));

    emit_cfun_call(code, janet_putindex);
    break;
  case JOP_LENGTH:
    if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + e].t == JANET_TUPLE) {
      /* printf("len tuple fast path possible\n"); */
      // e = collection
      // e -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (0 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, e * sizeof(Janet));
      // shift off tag bits
      // left 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE0);
      emit_byte(code, 0x11);
       // right 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE8);
      emit_byte(code, 0x11);
      // mov eax, rax + length_offset
      emit_byte(code, 0x8B);
      emit_byte(code, 0x80);
      emit_u32(code, (int32_t)(offsetof(JanetTupleHead, length) - offsetof(JanetTupleHead, data)));
      // eax -> xmm0
      emit_byte(code, 0xF2);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2A);
      emit_byte(code, 0xC0);
      emit_xmm_to_stack(code, a, 0);
    } else if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + e].t == JANET_STRING) {
      // e -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (0 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, e * sizeof(Janet));
      // shift off tag bits
      // left 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE0);
      emit_byte(code, 0x11);
       // right 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE8);
      emit_byte(code, 0x11);
      // mov eax, rax + length_offset
      emit_byte(code, 0x8B);
      emit_byte(code, 0x80);
      emit_u32(code, (int32_t)(offsetof(JanetStringHead, length) - offsetof(JanetStringHead, data)));
      // eax -> xmm0
      emit_byte(code, 0xF2);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2A);
      emit_byte(code, 0xC0);
      emit_xmm_to_stack(code, a, 0);
    } else if (ENABLE_DATAFLOW_TYPESPECIALIZATION && jitted->flow[(pc * fn->def->slotcount) + e].t == JANET_BUFFER) {
      // e -> rax
      emit_byte(code, 0x48);
      emit_byte(code, 0x8B);
      emit_byte(code, 0x84 + (0 << 3));
      emit_byte(code, 0x24);
      emit_u32(code, e * sizeof(Janet));
      // shift off tag bits
      // left 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE0);
      emit_byte(code, 0x11);
      // right 17
      emit_byte(code, 0x48);
      emit_byte(code, 0xC1);
      emit_byte(code, 0xE8);
      emit_byte(code, 0x11);
      // mov eax, rax + length_offset
      emit_byte(code, 0x8B);
      emit_byte(code, 0x80);
      emit_u32(code, (int32_t)(offsetof(JanetBuffer, count)));
      // eax -> xmm0
      emit_byte(code, 0xF2);
      emit_byte(code, 0x0F);
      emit_byte(code, 0x2A);
      emit_byte(code, 0xC0);
      emit_xmm_to_stack(code, a, 0);
    } else {
      // e = collection
      emit_stack_to_arg(code, 0, e);
      emit_cfun_call(code, janet_lengthv);
      emit_store_ret(code, a);
    }
    break;
  case JOP_MAKE_ARRAY:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_array);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_TUPLE:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_tuple);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_BRACKET_TUPLE:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_bracket_tuple);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_BUFFER:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_buffer);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_STRING:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_string);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_STRUCT:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_struct);
    emit_store_ret(code, a);
    break;
  case JOP_MAKE_TABLE:
    emit_non_janet_to_arg(code, 0, call_args_loc);
    emit_cfun_call(code, jit_make_table);
    emit_store_ret(code, a);
    break;
  case JOP_NEXT:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_stack_to_arg(code, 0, b);
    emit_stack_to_arg(code, 1, c);
    emit_cfun_call(code, janet_next);
    emit_store_ret(code, a);
    break;
  case JOP_CANCEL:
    janet_panic("janet's cancel opcode is not supported");
    break;
  default:
    janet_panic("unsupported op");
    return;
  }
}

int jitted_compile(JittedFunction *jitted) {
  JanetFunction *fn = jitted->fallback;
  JanetFuncDef *def = fn->def;
  size_t arity = def->arity;
  size_t bc_len = def->bytecode_length;
  int32_t def_slots = def->slotcount;

  CodeBuffer code = {
    malloc(256 * sizeof(uint8_t)),
    0,
    256,
    malloc(256 * sizeof(uint32_t)),
    malloc(256 * sizeof(uint32_t)),
    0
  };

  int def_size = def_slots * sizeof(Janet) + 1 * sizeof(CallArgs*);
  int stack_size = ((def_size + 8 + 15) & ~15) - 8;

  if (stack_size > 0) {
    // stack adjust
    emit_byte(&code, 0x48);             // REX.W: use 64-bit operands.
    emit_byte(&code, 0x81);             // Group 1 arithmetic on r/m64 with imm32.
    emit_byte(&code, 0xEC);             // 0xEC: sub rsp, imm32; 0xC4: add rsp, imm32.
    emit_u32(&code, stack_size); // Stack-frame size.
  }

  emit_arg_to_frame(&code, 2, def_size - 1 * sizeof(CallArgs*));
  // copy args to stack frame indexes
  for (int i = 0; i < arity; i++) {
    uint32_t offset = i * sizeof(Janet);
    // argv[offset] -> RAX
    emit_byte(&code, 0x48);
    emit_byte(&code, 0x8B); // MOV rax [rsi + offset]
    emit_byte(&code, 0x86);
    emit_u32(&code, offset);

    // RAX -> stack + offset
    emit_byte(&code, 0x48);
    emit_byte(&code, 0x89);
    emit_byte(&code, 0x84);
    emit_byte(&code, 0x24);
    emit_u32(&code, offset);
  }

  int *instruction_byte_locations = malloc(bc_len * sizeof(int));
  // copile bytecode
  for (int i = 0; i < bc_len; i++) {
    instruction_byte_locations[i] = code.count;
    compile_bytecode(&code, jitted, i, def->bytecode[i], stack_size);
  }

  // patch jumps
  for (int i = 0; i < code.jump_index; i++) {
    int dest = instruction_byte_locations[code.jump_targets[i]];

    int distance = dest - (code.jump_locations[i] + 4);
    for (int shift = 0; shift < 32; shift += 8) {
      code.data[code.jump_locations[i] + shift / 8] = (distance >> shift);
    }
  }

  void * mapping = mmap(NULL,
			code.count,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);

  if (mapping == MAP_FAILED) {
    int error = errno;
    free(code.data);
    janet_panicf("could not allocate JIT memory: %s", strerror(error));
  }

  memcpy(mapping, code.data, code.count);

  if (mprotect(mapping, code.count, PROT_READ | PROT_EXEC) != 0) {
    int error = errno;
    munmap(mapping, code.count);
    janet_panicf("could not make JIT memory executable: %s", strerror(error));
  }

  jitted->code = mapping;
  jitted->code_size = code.count;
  free(code.data);
  return 0;
}

static Janet jitted_function_call(void *p, int32_t argc, Janet *argv) {
  // for now, assume we are only doing math and all args are always numbers
  // we also assume that Janet objects are always nonboxed
  JittedFunction *jitted = p;

  if (jitted->code == NULL) {
    dataflow(jitted, argc, argv);
    // record signature
    jitted->signature_argc = argc;
    jitted->signature_arg_types = malloc(argc * sizeof(JanetType));
    for (int i = 0; i < argc; i++) {
      jitted->signature_arg_types[i] = janet_type(argv[i]);
    }
    jitted_compile(jitted);
  }

  if (argc == jitted->signature_argc) {
    for (int i = 0; i < argc; i++) {
      if (janet_type(argv[i]) != jitted->signature_arg_types[i]) {
	janet_panic("mismatching signature!");
      }
    }
    CallArgs ca = { 0, 0, NULL };
    Janet res = ((JitFn)jitted->code)(argc, argv, &ca);

    if (ca.capacity > 0) {
      free(ca.argv);
    }

    return res;
  } else {
    janet_panic("mismatching signature!");
  }
}

static const JanetAbstractType jitted_function_type = {
  "jittable-function",
  jitted_function_gc,
  jitted_function_gcmark,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  jitted_function_call,
  NULL,
  NULL,
  NULL
};

static Janet jit_jitable(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  JanetFunction *fn = janet_getfunction(argv, 0);
  if (fn->def->min_arity != fn->def->max_arity) {
    janet_panic("only fixed arity functions are supported (today)");
  }

  // todo: disqualify variable arg functions for now
  if (cfun_info_count == 0) {
    cfun_info[cfun_info_count].cfun = janet_unwrap_cfunction(janet_resolve_core("math/sin"));
    cfun_info[cfun_info_count++].result = JANET_NUMBER;
    cfun_info[cfun_info_count].cfun = janet_unwrap_cfunction(janet_resolve_core("math/cos"));
    cfun_info[cfun_info_count++].result = JANET_NUMBER;
    cfun_info[cfun_info_count].cfun = janet_unwrap_cfunction(janet_resolve_core("type"));
    cfun_info[cfun_info_count++].result = JANET_KEYWORD;
  }

  if (fun_info_count == 0) {
    fun_info[fun_info_count].fun = janet_unwrap_function(janet_resolve_core("dec"));
    fun_info[fun_info_count++].result = JANET_NUMBER;
  }

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  jitted->code = NULL;
  jitted->fallback = fn;
  return janet_wrap_abstract(jitted);
}

static Janet jit_compiled(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  JittedFunction *jitted = janet_getabstract(argv, 0, &jitted_function_type);
  if (jitted->code == NULL) {
    return janet_wrap_false();
  } else {
    return janet_wrap_true();
  }
}

static const JanetReg cfuns[] = {
  {"jitable", jit_jitable, "(jit/jitable function)\n\nReturns a Jittable version of a function. Jit compilation happens on first call."},
  {"compiled?", jit_compiled, "(jit/compiled? function)\n\nReturns if the function successfully compiled."},
  {NULL, NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
  janet_cfuns(env, "jit", cfuns);
  janet_register_abstract_type(&jitted_function_type);
}
