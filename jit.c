#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include "janet.h"

typedef struct BasicBlock BasicBlock;

typedef enum {
  OPERAND_UNUSED,
  OPERAND_UNDEFINED,
  OPERAND_VIRTUAL_REGISTER,
  /* OPERAND_PHYSICAL_REGISTER, */
  OPERAND_BASIC_BLOCK,
  OPERAND_IMM,
  OPERAND_JANET_IMM,
} OperandType;

typedef struct {
  OperandType type;
  union {
    size_t bb;
    size_t virtual_register;
    /* size_t physical_register; */
    uint64_t imm;
  };
} Operand;

typedef enum {
  JIT_INSTR_NOOP,
  JIT_INSTR_ERROR,
  JIT_INSTR_TYPECHECK,
  JIT_INSTR_RETURN,
  JIT_INSTR_ADD,
  JIT_INSTR_SUB,
  JIT_INSTR_MUL,
  JIT_INSTR_DIV,
  JIT_INSTR_DIV_FLOOR,
  JIT_INSTR_MODULO,
  JIT_INSTR_REAMINDER,
  JIT_INSTR_AND,
  JIT_INSTR_OR,
  JIT_INSTR_XOR,
  JIT_INSTR_NOT,
  JIT_INSTR_LSHIFT,
  JIT_INSTR_RSHIFT,
  JIT_INSTR_RUSHIFT,
  JIT_INSTR_MOV,
  JIT_INSTR_JUMP,
  JIT_INSTR_JUMP_IF,
  JIT_INSTR_JUMP_IF_NOT,
  JIT_INSTR_JUMP_IF_NIL,
  JIT_INSTR_JUMP_IF_NOT_NIL,
  JIT_INSTR_GREATER_THAN,
  JIT_INSTR_LESS_THAN,
  JIT_INSTR_EQUALS,
  JIT_INSTR_COMPARE,
  JIT_INSTR_LOAD,
  JIT_INSTR_LOAD_ARG,
  JIT_INSTR_PUSH,
  JIT_INSTR_CALL,
  JIT_INSTR_TAIL_CALL,
  JIT_INSTR_IN,
  JIT_INSTR_GET,
  JIT_INSTR_PUT,
  JIT_INSTR_LENGTH,
  JIT_INSTR_MAKE_ARRAY,
  JIT_INSTR_MAKE_BUFFER,
  JIT_INSTR_MAKE_STRING,
  JIT_INSTR_MAKE_STRUCT,
  JIT_INSTR_MAKE_TABLE,
  JIT_INSTR_MAKE_TUPLE,
  JIT_INSTR_MAKE_BRACKET_TUPLE,
  JIT_INSTR_GREATER_THAN_EQUAL,
  JIT_INSTR_LESS_THAN_EQUAL,
  JIT_INSTR_NEXT,
  JIT_INSTR_NOT_EQUAL, // maps to JOP_NOT_EQUALS, but I can't deal with this being the only equal(s)
  JIT_INSTR_PHI,
  JIT_INSTR_PHI_PLACEHOLDER,
} InstructionType;

char *instruction_names[] = {
  "NOOP",
  "ERROR",
  "TYPECHECK",
  "RETURN",
  "ADD",
  "SUB",
  "MUL",
  "DIV",
  "DIV_FLOOR",
  "MODULO",
  "REAMINDER",
  "AND",
  "OR",
  "XOR",
  "NOT",
  "LSHIFT",
  "RSHIFT",
  "RUSHIFT",
  "MOV",
  "JUMP",
  "JUMP_IF",
  "JUMP_IF_NOT",
  "JUMP_IF_NIL",
  "JUMP_IF_NOT_NIL",
  "GREATER_THAN",
  "LESS_THAN",
  "EQUALS",
  "COMPARE",
  "LOAD",
  "LOAD_ARG",
  "PUSH",
  "CALL",
  "TAIL_CALL",
  "IN",
  "GET",
  "PUT",
  "LENGTH",
  "MAKE_ARRAY",
  "MAKE_BUFFER",
  "MAKE_STRING",
  "MAKE_STRUCT",
  "MAKE_TABLE",
  "MAKE_TUPLE",
  "MAKE_BRACKET_TUPLE",
  "GREATER_THAN_EQUAL",
  "LESS_THAN_EQUAL",
  "NEXT",
  "NOT_EQUALS",
  "PHI",
  "PHI_PLACEHOLDER",
};

typedef struct {
  InstructionType type;
  Operand result;
  Operand operand1;
  Operand operand2;
  size_t phi_placeholder_slot;
} Instruction;

// This is silly as a struct
typedef struct {
  size_t basic_block_id;
} Edge;

struct BasicBlock {
  bool complete;
  size_t start_pc;
  size_t finish_pc;
  size_t count;
  size_t capacity;
  Instruction *instructions;
  uint32_t *slot_map;
  size_t input_edge_count;
  size_t input_edge_capacity;
  Edge *input_edges;
  size_t output_edge_count;
  size_t output_edge_capacity;
  Edge *output_edges;

};

typedef struct {
  size_t count;
  size_t capacity;
  BasicBlock *blocks;
  size_t *block_start_pcs; // leader pc -> block_id
  size_t virtual_register_count;
  size_t edge_count;
  // Operand *args?
} MethodBlocks;

typedef struct {
  size_t code_size;
  void *code;
  JanetFunction *fallback;
  size_t signature_argc;
  JanetType *signature_arg_types;
  MethodBlocks method_blocks;
} JittedFunction;

void setup_method_blocks(MethodBlocks *blocks, size_t bytecode_length) {
  blocks->count = 0;
  blocks->capacity = 256;
  blocks->blocks = malloc(blocks->capacity * sizeof(BasicBlock));
  blocks->block_start_pcs = malloc(bytecode_length * sizeof(size_t));
  blocks->virtual_register_count = 0;
}

#define CUR_BLOCK blocks->blocks[block_id]

Instruction *add_instruction(MethodBlocks *blocks, size_t block_id, InstructionType type) {
  /* printf("curblock count %i\n", CUR_BLOCK.count); */
  Instruction *instruction = &CUR_BLOCK.instructions[CUR_BLOCK.count];

  instruction->type = type;
  instruction->result.type = OPERAND_UNUSED;
  instruction->operand1.type = OPERAND_UNUSED;
  instruction->operand2.type = OPERAND_UNUSED;
  CUR_BLOCK.count++;
  return instruction;
}

#undef CUR_BLOCK

void add_edge(MethodBlocks *blocks, size_t source_bb, size_t target_bb) {
  blocks->blocks[source_bb].output_edges[blocks->blocks[source_bb].output_edge_count++].basic_block_id = target_bb;
  blocks->blocks[target_bb].input_edges[blocks->blocks[target_bb].input_edge_count++].basic_block_id = source_bb;
}

size_t new_basic_block(MethodBlocks *blocks, size_t start_pc) {
  size_t block_id = blocks->count;
  blocks->count++;

  blocks->blocks[block_id].complete = false;
  blocks->blocks[block_id].start_pc = start_pc;
  blocks->blocks[block_id].count = 0;
  blocks->blocks[block_id].capacity = 256;
  blocks->blocks[block_id].instructions = malloc(blocks->blocks[0].capacity * sizeof(Instruction));

  blocks->blocks[block_id].slot_map = malloc(256 * sizeof(uint32_t));

  blocks->blocks[block_id].input_edge_count = 0;
  blocks->blocks[block_id].input_edge_capacity = 12;
  blocks->blocks[block_id].input_edges = malloc(blocks->blocks[block_id].input_edge_capacity * sizeof(Edge));
  blocks->blocks[block_id].output_edge_count = 0;
  blocks->blocks[block_id].output_edge_capacity = 12;
  blocks->blocks[block_id].output_edges = malloc(blocks->blocks[block_id].output_edge_capacity * sizeof(Edge));
  return block_id;
}

#define AA ((instr >> 8)  & 0xFF)
#define BB ((instr >> 16) & 0xFF)
#define CC (instr >> 24)
#define DD (instr >> 8)
#define EE (instr >> 16)

static void compile_bb_bytecode(MethodBlocks *blocks, JanetFunction *fn, size_t block_id, uint32_t *parent_slot_map) {
  JanetFuncDef *def = fn->def;

  // keep my own copy of slot_map to avoid changing the owners copy
  uint32_t *slot_map = malloc(def->slotcount * sizeof(uint32_t));
  if (blocks->blocks[block_id].input_edge_count == 0) {
    memcpy(slot_map, parent_slot_map, def->slotcount * sizeof(uint32_t));
  } else {
    size_t input_block_id = blocks->blocks[block_id].input_edges[0].basic_block_id;
    memcpy(slot_map, blocks->blocks[input_block_id].slot_map, def->slotcount * sizeof(uint32_t));
  }
  // emit phi for each change from the first predecessor's map.
  for (size_t slot_i = 0; slot_i < def->slotcount; slot_i++) {
    for (size_t block_i = 1; block_i < blocks->blocks[block_id].input_edge_count; block_i++) {
      size_t input_block_id = blocks->blocks[block_id].input_edges[block_i].basic_block_id;
      if (blocks->blocks[input_block_id].slot_map[slot_i] != blocks->blocks[blocks->blocks[block_id].input_edges[0].basic_block_id].slot_map[slot_i]) {
	Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PHI_PLACEHOLDER);

	if (blocks->blocks[input_block_id].complete && blocks->blocks[blocks->blocks[block_id].input_edges[0].basic_block_id].complete) {
	  instruction->type = JIT_INSTR_PHI;

	  instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
	  instruction->operand1.virtual_register = slot_map[slot_i];

	  instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
	  instruction->operand2.virtual_register = blocks->blocks[input_block_id].slot_map[slot_i];
	} else {
	  instruction->operand1.type = OPERAND_BASIC_BLOCK;
	  instruction->operand1.bb = blocks->blocks[block_id].input_edges[0].basic_block_id;

	  instruction->operand2.type = OPERAND_BASIC_BLOCK;
	  instruction->operand2.bb = input_block_id;

	  instruction->phi_placeholder_slot = slot_i;
	  /* instruction->result.type = OPERAND_VIRTUAL_REGISTER; */
	  /* instruction->result.virtual_register = slot_i; */
	}
	slot_map[slot_i] = blocks->virtual_register_count;
	instruction->result.type = OPERAND_VIRTUAL_REGISTER;
	instruction->result.virtual_register = blocks->virtual_register_count++;
      }
    }
  }

  blocks->blocks[block_id].slot_map = slot_map;

  // TODO: is complete needed anymore?
  blocks->blocks[block_id].complete = true;
  for (size_t index = blocks->blocks[block_id].start_pc; index < blocks->blocks[block_id].finish_pc; index++) {
    uint32_t instr = def->bytecode[index];
    switch (instr & 0xFF) {
    case JOP_NOOP:
      break;
    case JOP_ERROR: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_ERROR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->operand1.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_TYPECHECK: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_TYPECHECK);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->operand1.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_RETURN: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RETURN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      return;
    }
    case JOP_RETURN_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RETURN);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(janet_wrap_nil());
      return;
    }
    case JOP_ADD_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_ADD);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_ADD: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_ADD);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SUBTRACT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_SUB);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SUBTRACT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_SUB);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MULTIPLY_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MUL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = janet_u64(janet_wrap_number(CC));
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MULTIPLY: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MUL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_DIV);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_DIV);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE_FLOOR: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_DIV_FLOOR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MODULO: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MODULO);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_REMAINDER: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_REAMINDER);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BAND: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_AND);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BOR: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_OR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BXOR: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_XOR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BNOT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_NOT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_LEFT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_LEFT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_UNSIGNED: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RUSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_RUSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MOVE_FAR: {
      slot_map[EE] = slot_map[AA];
      break;
    }
    case JOP_MOVE_NEAR: {
      slot_map[AA] = slot_map[EE];
      break;
    }
    case JOP_JUMP: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP);
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 8)];
      return;
    }
    case JOP_JUMP_IF: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP_IF);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NOT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP_IF_NOT);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP_IF_NIL);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NOT_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP_IF_NOT_NIL);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_GREATER_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_GREATER_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_GREATER_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_GREATER_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LESS_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LESS_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_COMPARE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_COMPARE);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(janet_wrap_nil());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_TRUE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(janet_wrap_true());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_FALSE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(janet_wrap_false());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_INTEGER: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_IMM;
      instruction->operand1.imm = EE;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_CONSTANT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(def->constants[EE]);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_UPVALUE: {
      janet_panic("janet's upvalue opcode is not supported");
      break;
    }
    case JOP_LOAD_SELF: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.imm = janet_u64(janet_wrap_function(fn));
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SET_UPVALUE: {
      janet_panic("janet's set upvalue opcode is not supported");
      break;
    }
    case JOP_CLOSURE: {
      janet_panic("janet's closure opcode is not supported");
      break;
    }
    case JOP_PUSH: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUSH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[DD];
      break;
    }
    case JOP_PUSH_2: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUSH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[EE];
      break;
    }
    case JOP_PUSH_3: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUSH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[BB];

      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[CC];
      break;
    }
    case JOP_PUSH_ARRAY: {
      janet_panic("janet's push array is not yet supported");
      break;
    }
    case JOP_CALL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_CALL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];

      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_TAILCALL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_TAIL_CALL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];

      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_RESUME: {
      janet_panic("janet's resume is not supported");
      break;
    }
    case JOP_SIGNAL: {
      janet_panic("janet's signal is not supported");
      break;
    }
    case JOP_PROPAGATE: {
      janet_panic("janet's propagate is not supported");
      break;
    }
    case JOP_IN: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_IN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_GET: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_GET);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_PUT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[BB];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[CC];
      break;
    }
    case JOP_GET_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_GET);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_PUT_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[BB];
      break;
    }
    case JOP_LENGTH: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_PUT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_ARRAY: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_ARRAY);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_BUFFER: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_BUFFER);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_STRING: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_STRING);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_STRUCT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_STRUCT);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_TABLE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_TABLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_TUPLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_MAKE_BRACKET_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_MAKE_BRACKET_TUPLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      instruction->result.virtual_register = slot_map[AA];
      break;
    }
    case JOP_GREATER_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_GREATER_THAN_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_LESS_THAN_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_NEXT: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_NEXT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      break;
    }
    case JOP_NOT_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_NOT_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_NOT_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_NOT_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_IMM;
      instruction->operand2.imm = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_CANCEL: {
      janet_panic("janet's resume is not supported");
      break;
    }
    }
  }
  // reached end due to leader / jump target instruction
  Instruction *instruction = add_instruction(blocks, block_id, JIT_INSTR_JUMP);
  instruction->operand1.type = OPERAND_BASIC_BLOCK;
  instruction->operand1.bb = blocks->block_start_pcs[blocks->blocks[block_id].finish_pc];
}

static void rpo_order(MethodBlocks *blocks, size_t block_id, bool *visited, size_t *list, size_t *count) {
  if (visited[block_id]) { return; }
  visited[block_id] = true;
  for (size_t i = 0; i < blocks->blocks[block_id].output_edge_count; i++) {
    rpo_order(blocks, blocks->blocks[block_id].output_edges[i].basic_block_id, visited, list, count);
  }

  list[(*count)++] = block_id;
}

static void replace_virtual_register(MethodBlocks *blocks, size_t slotcount, size_t old, size_t new) {
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    for (size_t slot_i = 0; slot_i < slotcount; slot_i++) {
      if (block->slot_map[slot_i] == old) {
	block->slot_map[slot_i] = new;
      }
    }

    for (size_t i = 0; i < block->count; i++) {
      Instruction *instr = &block->instructions[i];
      if (instr->operand1.type == OPERAND_VIRTUAL_REGISTER && instr->operand1.virtual_register == old) {
	instr->operand1.virtual_register = new;
      }

      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER && instr->operand2.virtual_register == old) {
	instr->operand2.virtual_register = new;
      }

      switch (instr->type) {
      case JIT_INSTR_JUMP_IF:
      case JIT_INSTR_JUMP_IF_NOT:
      case JIT_INSTR_JUMP_IF_NIL:
      case JIT_INSTR_JUMP_IF_NOT_NIL: {
	if (instr->result.type == OPERAND_VIRTUAL_REGISTER && instr->result.virtual_register == old) {
	  instr->result.virtual_register = new;
	}
	break;
      }
      default:
	// noop
	break;
      }


    }
  }
}

void build_basic_blocks(MethodBlocks *blocks, JanetFunction *fn) {
  // mark leaders
  bool *leaders = calloc(256, sizeof(bool));
  leaders[0] = true;
  for (int i = 0; i < fn->def->bytecode_length; i++) {
    uint32_t instr = fn->def->bytecode[i];
    switch (instr & 0xFF) {
    case JOP_JUMP: {
      leaders[i + ((int32_t)instr >> 8)] = true;
      break;
    }
    case JOP_JUMP_IF:
    case JOP_JUMP_IF_NOT:
    case JOP_JUMP_IF_NIL:
    case JOP_JUMP_IF_NOT_NIL: {
      leaders[i + ((int32_t)instr >> 16)] = true;
      leaders[i + 1] = true;
      break;
    }
    }
  }

  // create basic blocks
  int32_t cur_block = -1;
  for (int pc = 0; pc < fn->def->bytecode_length; pc++) {
    blocks->block_start_pcs[pc] = -1;
    if (leaders[pc]) {
      blocks->block_start_pcs[pc] = new_basic_block(blocks, pc);
      if (cur_block >= 0) {
	blocks->blocks[cur_block].finish_pc = pc;
      }
      cur_block = blocks->block_start_pcs[pc];
    }
  }
  if (cur_block >= 0) {
    blocks->blocks[cur_block].finish_pc = fn->def->bytecode_length;
  }

  // add edges between basic blocks
  for (size_t block_id = 0; block_id < blocks->count; block_id++) {
    uint32_t last_op = blocks->blocks[block_id].finish_pc - 1;
    uint64_t instr = fn->def->bytecode[last_op];
    switch (instr & 0xFF) {
    case JOP_JUMP: {
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + ((int32_t)instr >> 8)]);
      break;
    }
    case JOP_JUMP_IF:
    case JOP_JUMP_IF_NOT:
    case JOP_JUMP_IF_NIL:
    case JOP_JUMP_IF_NOT_NIL: {
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + ((int32_t)instr >> 16)]);
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + 1]);
      break;
    }
    case JOP_RETURN:
    case JOP_RETURN_NIL:
    case JOP_ERROR:
    case JOP_TAILCALL:
      break;
    default:
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + 1]);
    }
  }

  // TODO: get the real size here
  // emit arg loads
  uint32_t *slot_map = malloc(fn->def->slotcount * sizeof(uint32_t));
  for (int slot_i = 0; slot_i < fn->def->slotcount; slot_i++) {
    slot_map[slot_i] = UINT32_MAX;
  }
  for (int i = 0; i < fn->def->arity; i++) {
    Instruction *instruction = add_instruction(blocks, 0, JIT_INSTR_LOAD_ARG);
    instruction->operand1.type = OPERAND_IMM;
    instruction->operand1.imm = i;
    instruction->result.type = OPERAND_VIRTUAL_REGISTER;
    slot_map[i] = blocks->virtual_register_count;
    instruction->result.virtual_register = blocks->virtual_register_count++;
  }

  // rpo order
  size_t *list = malloc(blocks->count * sizeof(size_t));
  bool *visited = calloc(blocks->count, sizeof(bool));
  size_t count = 0;
  rpo_order(blocks, 0, visited, list, &count);

  while (count > 0) {
    compile_bb_bytecode(blocks, fn, list[--count], slot_map);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t block_i = 0; block_i < blocks->count; block_i++) {
      for (size_t instr_i = 0; instr_i < blocks->blocks[block_i].count; instr_i++) {
	Instruction *instr = &blocks->blocks[block_i].instructions[instr_i];

	if (blocks->blocks[block_i].instructions[instr_i].type == JIT_INSTR_PHI_PLACEHOLDER) {
	  // placeholder result = comflict recister, op1 = bb op2 = bb
	  size_t slot_i = instr->phi_placeholder_slot;
	  size_t bb1 = instr->operand1.bb;
	  size_t bb2 = instr->operand2.bb;

	  instr->type = JIT_INSTR_PHI;
	  if (blocks->blocks[bb1].slot_map[slot_i] == UINT32_MAX) {
	    instr->operand1.type = OPERAND_UNDEFINED;
	  } else {
	    instr->operand1.type = OPERAND_VIRTUAL_REGISTER;
	    instr->operand1.virtual_register = blocks->blocks[bb1].slot_map[slot_i];
	  }
	  if (blocks->blocks[bb2].slot_map[slot_i] == UINT32_MAX) {
	    instr->operand2.type = OPERAND_UNDEFINED;
	  } else {
	    instr->operand2.type = OPERAND_VIRTUAL_REGISTER;
	    instr->operand2.virtual_register = blocks->blocks[bb2].slot_map[slot_i];
	  }
	}

	if (blocks->blocks[block_i].instructions[instr_i].type == JIT_INSTR_PHI) {
	  if (instr->operand1.type == OPERAND_UNDEFINED && instr->operand2.type == OPERAND_UNDEFINED) {
	    instr->type = JIT_INSTR_NOOP;
	  } else if (instr->operand1.type == OPERAND_UNDEFINED) {
	    instr->type = JIT_INSTR_NOOP;
	    changed = true;
	    replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, instr->operand2.virtual_register);
	  } else if (instr->operand2.type == OPERAND_UNDEFINED) {
	    instr->type = JIT_INSTR_NOOP;
	    changed = true;
	    replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, instr->operand1.virtual_register);
	  } else {
	    if (instr->operand1.virtual_register == instr->operand2.virtual_register) {
	      instr->type = JIT_INSTR_NOOP;
	      changed = true;
	      replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, instr->operand1.virtual_register);
	    }
	    if (instr->operand1.virtual_register == instr->result.virtual_register) {
	      instr->type = JIT_INSTR_NOOP;
	      changed = true;
	      replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, instr->operand2.virtual_register);
	    }
	    if (instr->operand2.virtual_register == instr->result.virtual_register) {
	      instr->type = JIT_INSTR_NOOP;
	      changed = true;
	      replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, instr->operand1.virtual_register);
	    }
	  }
	}
      }
    }
  }
}

void print_ops(Operand *op) {
  switch (op->type) {
  case OPERAND_VIRTUAL_REGISTER:
    printf("v%zu", op->virtual_register);
    break;
  case OPERAND_BASIC_BLOCK:
    printf("bb%zu", op->bb);
    break;
  case OPERAND_IMM:
    printf("imm%zu", op->imm);
    break;
  case OPERAND_JANET_IMM:
    printf("jimm");
    break;
    case OPERAND_UNUSED:
    printf("_");
    break;
    case OPERAND_UNDEFINED:
    printf("?");
    break;
  }
}

void print_basic_blocks(MethodBlocks *blocks) {
  // todo print method name etc.
  for (int block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *bb = &blocks->blocks[block_i];
    printf("----block %i : %zu instructions ----\n", block_i, bb->count);
    for (int instr_i = 0; instr_i < bb->count; instr_i++) {
      Instruction *instruction = &bb->instructions[instr_i];
      printf("%i. %s [", instr_i, instruction_names[instruction->type]);
      print_ops(&instruction->result);
      printf(", ");
      print_ops(&instruction->operand1);
      printf(", ");
      print_ops(&instruction->operand2);
      printf("]\n");
    }
    printf("-----------------\n");
  }
}

/* typedef Janet (*JitFn)(int32_t argc, Janet *argv, CallArgs *call_args); */

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

static Janet jitted_function_call(void *p, int32_t argc, Janet *argv) {
  JittedFunction *jitted = p;
  printf("JIT CALLED\n");
  return janet_call(jitted->fallback, argc, argv);
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
    janet_panic("only fixed arity functions are supported");
  }

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  jitted->code_size = 0;
  jitted->code = NULL;
  jitted->fallback = fn;
  jitted->signature_argc = fn->def->min_arity;
  jitted->signature_arg_types = NULL;
  setup_method_blocks(&jitted->method_blocks, fn->def->bytecode_length);
  build_basic_blocks(&jitted->method_blocks, fn);
  print_basic_blocks(&jitted->method_blocks);

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
