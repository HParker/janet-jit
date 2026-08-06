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

typedef Janet (*JitFn)(int32_t argc, Janet *argv);

typedef struct {
  void *code;
  size_t code_size;
  JanetFunction *fallback;
} JittedFunction;



typedef struct {
  uint8_t *data;
  size_t count;
  size_t capacity;
} CodeBuffer;

static int jitted_function_gc(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  if (jitted->code != NULL) {
    munmap(jitted->code, jitted->code_size);
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

static void compile_bytecode(CodeBuffer *code, uint32_t instr, int stack_size) {
  // TODO: these can be #define/macros, but this is fine for now
  int opcode = instr & 0xFF;
  int a = (instr >> 8) & 0xFF;
  int b = (instr >> 16) & 0xFF;
  int c = (instr >> 24) & 0xFF;
  int32_t immediate = (int32_t)instr >> 16;
  uint32_t d = (uint32_t)instr >> 8;
  uint32_t e = (uint32_t)instr >> 16;

  switch (opcode) {
  case JOP_NOOP:
    break;
  case JOP_LOAD_INTEGER:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(immediate)));

    // mov rsp + offset, rax
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;

  case JOP_ADD:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x58, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SUBTRACT:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MULTIPLY:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x59, a, 0, 1);
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
  case JOP_MODULO:
    // TODO
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
    emit_byte(code, 0x03);
    // xmm1 * xmm2
    emit_binary_op(code, 0x59, a, 1, 2);
    // xmm0 - xmm1
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
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

  default:
    janet_panic("unsupported op");
    return;
  }
}

int jitted_compile(JittedFunction *jitted, JanetFuncDef *def) {
  size_t arity = def->arity;
  size_t bc_len = def->bytecode_length;
  int32_t def_slots = def->slotcount;

  CodeBuffer code = {
    malloc(256 * sizeof(uint8_t)),
    0,
    256
  };

  int stack_size = (arity + def_slots) * sizeof(Janet);

  if (stack_size > 0) {
    // stack adjust
    emit_byte(&code, 0x48);             // REX.W: use 64-bit operands.
    emit_byte(&code, 0x81);             // Group 1 arithmetic on r/m64 with imm32.
    emit_byte(&code, 0xEC);             // 0xEC: sub rsp, imm32; 0xC4: add rsp, imm32.
    emit_u32(&code, stack_size); // Stack-frame size.
  }

  // copy args to stack frame indexes
  for (int i = 0; i < arity; i++) {
    uint32_t offset = i * sizeof(Janet);
    // argv[offset] -> RAX
    emit_byte(&code, 0x48);
    emit_byte(&code, 0x8B); // MOV rax [rsi + offset]
    emit_byte(&code, 0x86);
    emit_u32(&code, offset);

    // RAX -> stack + offset
    emit_byte(&code, 0x48);   // REX.W: move all 64 bits.
    emit_byte(&code, 0x89);   // MOV r64 into r/m64.
    emit_byte(&code, 0x84);   // ModR/M: disp32 address, source RAX, SIB follows.
    emit_byte(&code, 0x24);   // SIB: scale 1, no index, base RSP.
    emit_u32(&code, offset);  // disp32: byte offset of the Janet VM slot.
  }

  // copile bytecode
  for (int i = 0; i < bc_len; i++) {
    compile_bytecode(&code, def->bytecode[i], stack_size);
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
    uint8_t *code = malloc(256 * sizeof(uint8_t));
    jitted_compile(jitted, jitted->fallback->def);
  }

  return ((JitFn)jitted->code)(argc, argv);
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

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  jitted->code = NULL;
  jitted->fallback = janet_getfunction(argv, 0);

  return janet_wrap_abstract(jitted);
}

static Janet jit_compiled(int32_t argc, Janet *argv) {
  return janet_wrap_true();
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
