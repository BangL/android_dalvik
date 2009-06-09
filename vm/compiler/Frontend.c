/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Dalvik.h"
#include "libdex/OpCode.h"
#include "dexdump/OpCodeNames.h"
#include "interp/Jit.h"
#include "CompilerInternals.h"

/*
 * Parse an instruction, return the length of the instruction
 */
static inline int parseInsn(const u2 *codePtr, DecodedInstruction *decInsn,
                            bool printMe)
{
    u2 instr = *codePtr;
    OpCode opcode = instr & 0xff;
    int insnWidth;

    // Need to check if this is a real NOP or a pseudo opcode
    if (opcode == OP_NOP && instr != 0) {
        if (instr == kPackedSwitchSignature) {
            insnWidth = 4 + codePtr[1] * 2;
        } else if (instr == kSparseSwitchSignature) {
            insnWidth = 2 + codePtr[1] * 4;
        } else if (instr == kArrayDataSignature) {
            int width = codePtr[1];
            int size = codePtr[2] | (codePtr[3] << 16);
            // The plus 1 is to round up for odd size and width
            insnWidth = 4 + ((size * width) + 1) / 2;
        }
        insnWidth = 0;
    } else {
        insnWidth = gDvm.instrWidth[opcode];
        if (insnWidth < 0) {
            insnWidth = -insnWidth;
        }
    }

    dexDecodeInstruction(gDvm.instrFormat, codePtr, decInsn);
    if (printMe) {
        LOGD("%p: %#06x %s\n", codePtr, opcode, getOpcodeName(opcode));
    }
    return insnWidth;
}

/*
 * Identify block-ending instructions and collect supplemental information
 * regarding the following instructions.
 */
static inline bool findBlockBoundary(const Method *caller, MIR *insn,
                                     unsigned int curOffset,
                                     unsigned int *target, bool *isInvoke,
                                     const Method **callee)
{
    switch (insn->dalvikInsn.opCode) {
        /* Target is not compile-time constant */
        case OP_RETURN_VOID:
        case OP_RETURN:
        case OP_RETURN_WIDE:
        case OP_RETURN_OBJECT:
        case OP_THROW:
        case OP_INVOKE_VIRTUAL:
        case OP_INVOKE_VIRTUAL_RANGE:
        case OP_INVOKE_INTERFACE:
        case OP_INVOKE_INTERFACE_RANGE:
        case OP_INVOKE_VIRTUAL_QUICK:
        case OP_INVOKE_VIRTUAL_QUICK_RANGE:
            *isInvoke = true;
            break;
        case OP_INVOKE_SUPER:
        case OP_INVOKE_SUPER_RANGE: {
            int mIndex = caller->clazz->pDvmDex->
                pResMethods[insn->dalvikInsn.vB]->methodIndex;
            const Method *calleeMethod =
                caller->clazz->super->vtable[mIndex];

            if (!dvmIsNativeMethod(calleeMethod)) {
                *target = (unsigned int) calleeMethod->insns;
            }
            *isInvoke = true;
            *callee = calleeMethod;
            break;
        }
        case OP_INVOKE_STATIC:
        case OP_INVOKE_STATIC_RANGE: {
            const Method *calleeMethod =
                caller->clazz->pDvmDex->pResMethods[insn->dalvikInsn.vB];

            if (!dvmIsNativeMethod(calleeMethod)) {
                *target = (unsigned int) calleeMethod->insns;
            }
            *isInvoke = true;
            *callee = calleeMethod;
            break;
        }
        case OP_INVOKE_SUPER_QUICK:
        case OP_INVOKE_SUPER_QUICK_RANGE: {
            const Method *calleeMethod =
                caller->clazz->super->vtable[insn->dalvikInsn.vB];

            if (!dvmIsNativeMethod(calleeMethod)) {
                *target = (unsigned int) calleeMethod->insns;
            }
            *isInvoke = true;
            *callee = calleeMethod;
            break;
        }
        case OP_INVOKE_DIRECT:
        case OP_INVOKE_DIRECT_RANGE: {
            const Method *calleeMethod =
                caller->clazz->pDvmDex->pResMethods[insn->dalvikInsn.vB];
            if (!dvmIsNativeMethod(calleeMethod)) {
                *target = (unsigned int) calleeMethod->insns;
            }
            *isInvoke = true;
            *callee = calleeMethod;
            break;
        }
        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            *target = curOffset + (int) insn->dalvikInsn.vA;
            break;

        case OP_IF_EQ:
        case OP_IF_NE:
        case OP_IF_LT:
        case OP_IF_GE:
        case OP_IF_GT:
        case OP_IF_LE:
            *target = curOffset + (int) insn->dalvikInsn.vC;
            break;

        case OP_IF_EQZ:
        case OP_IF_NEZ:
        case OP_IF_LTZ:
        case OP_IF_GEZ:
        case OP_IF_GTZ:
        case OP_IF_LEZ:
            *target = curOffset + (int) insn->dalvikInsn.vB;
            break;

        default:
            return false;
    } return true;
}

/*
 * Identify conditional branch instructions
 */
static inline bool isUnconditionalBranch(MIR *insn)
{
    switch (insn->dalvikInsn.opCode) {
        case OP_RETURN_VOID:
        case OP_RETURN:
        case OP_RETURN_WIDE:
        case OP_RETURN_OBJECT:
        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            return true;
        default:
            return false;
    }
}

/*
 * Main entry point to start trace compilation. Basic blocks are constructed
 * first and they will be passed to the codegen routines to convert Dalvik
 * bytecode into machine code.
 */
void *dvmCompileTrace(JitTraceDescription *desc, int numMaxInsts)
{
    const DexCode *dexCode = dvmGetMethodCode(desc->method);
    const JitTraceRun* currRun = &desc->trace[0];
    unsigned int curOffset = currRun->frag.startOffset;
    unsigned int numInsts = currRun->frag.numInsts;
    const u2 *codePtr = dexCode->insns + curOffset;
    int traceSize = 0;
    const u2 *startCodePtr = codePtr;
    BasicBlock *startBB, *curBB, *lastBB;
    int numBlocks = 0;
    static int compilationId;
    CompilationUnit cUnit;
    memset(&cUnit, 0, sizeof(CompilationUnit));

    /* Initialize the printMe flag */
    cUnit.printMe = gDvmJit.printMe;

    /* Identify traces that we don't want to compile */
    if (gDvmJit.methodTable) {
        int len = strlen(desc->method->clazz->descriptor) +
                  strlen(desc->method->name) + 1;
        char *fullSignature = dvmCompilerNew(len, true);
        strcpy(fullSignature, desc->method->clazz->descriptor);
        strcat(fullSignature, desc->method->name);

        int hashValue = dvmComputeUtf8Hash(fullSignature);

        /*
         * Doing three levels of screening to see whether we want to skip
         * compiling this method
         */

        /* First, check the full "class;method" signature */
        bool methodFound =
            dvmHashTableLookup(gDvmJit.methodTable, hashValue,
                               fullSignature, (HashCompareFunc) strcmp,
                               false) !=
            NULL;

        /* Full signature not found - check the enclosing class */
        if (methodFound == false) {
            int hashValue = dvmComputeUtf8Hash(desc->method->clazz->descriptor);
            methodFound =
                dvmHashTableLookup(gDvmJit.methodTable, hashValue,
                               (char *) desc->method->clazz->descriptor,
                               (HashCompareFunc) strcmp, false) !=
                NULL;
            /* Enclosing class not found - check the method name */
            if (methodFound == false) {
                int hashValue = dvmComputeUtf8Hash(desc->method->name);
                methodFound =
                    dvmHashTableLookup(gDvmJit.methodTable, hashValue,
                                   (char *) desc->method->name,
                                   (HashCompareFunc) strcmp, false) !=
                    NULL;
            }
        }

        /*
         * Under the following conditions, the trace will be *conservatively*
         * compiled by only containing single-step instructions to and from the
         * interpreter.
         * 1) If includeSelectedMethod == false, the method matches the full or
         *    partial signature stored in the hash table.
         *
         * 2) If includeSelectedMethod == true, the method does not match the
         *    full and partial signature stored in the hash table.
         */
        if (gDvmJit.includeSelectedMethod != methodFound) {
            cUnit.allSingleStep = true;
        } else {
            /* Compile the trace as normal */

            /* Print the method we cherry picked */
            if (gDvmJit.includeSelectedMethod == true) {
                cUnit.printMe = true;
            }
        }
    }

    /* Allocate the first basic block */
    lastBB = startBB = curBB = dvmCompilerNewBB(DALVIK_BYTECODE);
    curBB->startOffset = curOffset;
    curBB->id = numBlocks++;

    if (cUnit.printMe) {
        LOGD("--------\nCompiler: Building trace for %s, offset 0x%x\n",
             desc->method->name, curOffset);
    }

    /*
     * Analyze the trace descriptor and include up to the maximal number
     * of Dalvik instructions into the IR.
     */
    while (1) {
        MIR *insn;
        int width;
        insn = dvmCompilerNew(sizeof(MIR),false);
        insn->offset = curOffset;
        width = parseInsn(codePtr, &insn->dalvikInsn, cUnit.printMe);
        insn->width = width;
        traceSize += width;
        dvmCompilerAppendMIR(curBB, insn);
        cUnit.numInsts++;
        /* Instruction limit reached - terminate the trace here */
        if (cUnit.numInsts >= numMaxInsts) {
            break;
        }
        if (--numInsts == 0) {
            if (currRun->frag.runEnd) {
                break;
            } else {
                curBB = dvmCompilerNewBB(DALVIK_BYTECODE);
                lastBB->next = curBB;
                lastBB = curBB;
                curBB->id = numBlocks++;
                currRun++;
                curOffset = currRun->frag.startOffset;
                numInsts = currRun->frag.numInsts;
                curBB->startOffset = curOffset;
                codePtr = dexCode->insns + curOffset;
            }
        } else {
            curOffset += width;
            codePtr += width;
        }
    }

    /*
     * Now scan basic blocks containing real code to connect the
     * taken/fallthrough links. Also create chaining cells for code not included
     * in the trace.
     */
    for (curBB = startBB; curBB; curBB = curBB->next) {
        MIR *lastInsn = curBB->lastMIRInsn;
        /* Hit a pseudo block - exit the search now */
        if (lastInsn == NULL) {
            break;
        }
        curOffset = lastInsn->offset;
        unsigned int targetOffset = curOffset;
        unsigned int fallThroughOffset = curOffset + lastInsn->width;
        bool isInvoke = false;
        const Method *callee = NULL;

        findBlockBoundary(desc->method, curBB->lastMIRInsn, curOffset,
                          &targetOffset, &isInvoke, &callee);

        /* Link the taken and fallthrough blocks */
        BasicBlock *searchBB;

        /* No backward branch in the trace - start searching the next BB */
        for (searchBB = curBB->next; searchBB; searchBB = searchBB->next) {
            if (targetOffset == searchBB->startOffset) {
                curBB->taken = searchBB;
            }
            if (fallThroughOffset == searchBB->startOffset) {
                curBB->fallThrough = searchBB;
            }
        }

        int flags = dexGetInstrFlags(gDvm.instrFlags,
                                     lastInsn->dalvikInsn.opCode);

        /*
         * Some blocks are ended by non-control-flow-change instructions,
         * currently only due to trace length constraint. In this case we need
         * to generate an explicit branch at the end of the block to jump to
         * the chaining cell.
         */
        curBB->needFallThroughBranch =
            (flags & (kInstrCanBranch | kInstrCanSwitch | kInstrCanReturn |
                      kInstrInvoke)) == 0;

        /* Target block not included in the trace */
        if (targetOffset != curOffset && curBB->taken == NULL) {
            if (isInvoke) {
                lastBB->next = dvmCompilerNewBB(CHAINING_CELL_INVOKE);
            /* For unconditional branches, request a hot chaining cell */
            } else {
                lastBB->next = dvmCompilerNewBB(flags & kInstrUnconditional ?
                                                  CHAINING_CELL_HOT :
                                                  CHAINING_CELL_NORMAL);
            }
            lastBB = lastBB->next;
            lastBB->id = numBlocks++;
            if (isInvoke) {
                lastBB->startOffset = 0;
                lastBB->containingMethod = callee;
            } else {
                lastBB->startOffset = targetOffset;
            }
            curBB->taken = lastBB;
        }

        /* Fallthrough block not included in the trace */
        if (!isUnconditionalBranch(lastInsn) && curBB->fallThrough == NULL) {
            /*
             * If the chaining cell is after an invoke or
             * instruction that cannot change the control flow, request a hot
             * chaining cell.
             */
            if (isInvoke || curBB->needFallThroughBranch) {
                lastBB->next = dvmCompilerNewBB(CHAINING_CELL_HOT);
            } else {
                lastBB->next = dvmCompilerNewBB(CHAINING_CELL_NORMAL);
            }
            lastBB = lastBB->next;
            lastBB->id = numBlocks++;
            lastBB->startOffset = fallThroughOffset;
            curBB->fallThrough = lastBB;
        }
    }

    /* Now create a special block to host PC reconstruction code */
    lastBB->next = dvmCompilerNewBB(PC_RECONSTRUCTION);
    lastBB = lastBB->next;
    lastBB->id = numBlocks++;

    /* And one final block that publishes the PC and raise the exception */
    lastBB->next = dvmCompilerNewBB(EXCEPTION_HANDLING);
    lastBB = lastBB->next;
    lastBB->id = numBlocks++;

    if (cUnit.printMe) {
        LOGD("TRACEINFO (%d): 0x%08x %s%s 0x%x %d of %d, %d blocks",
            compilationId++,
            (intptr_t) desc->method->insns,
            desc->method->clazz->descriptor,
            desc->method->name,
            desc->trace[0].frag.startOffset,
            traceSize,
            dexCode->insnsSize,
            numBlocks);
    }

    BasicBlock **blockList;

    cUnit.method = desc->method;
    cUnit.traceDesc = desc;
    cUnit.numBlocks = numBlocks;
    dvmInitGrowableList(&cUnit.pcReconstructionList, 8);
    blockList = cUnit.blockList =
        dvmCompilerNew(sizeof(BasicBlock *) * numBlocks, true);

    int i;

    for (i = 0, curBB = startBB; i < numBlocks; i++) {
        blockList[i] = curBB;
        curBB = curBB->next;
    }
    /* Make sure all blocks are added to the cUnit */
    assert(curBB == NULL);

    if (cUnit.printMe) {
        dvmCompilerDumpCompilationUnit(&cUnit);
    }

    /* Convert MIR to LIR, etc. */
    dvmCompilerMIR2LIR(&cUnit);

    /* Convert LIR into machine code. */
    dvmCompilerAssembleLIR(&cUnit);

    if (cUnit.printMe) {
        if (cUnit.halveInstCount) {
            LOGD("Assembler aborted");
        } else {
            dvmCompilerCodegenDump(&cUnit);
        }
        LOGD("End %s%s, %d Dalvik instructions",
             desc->method->clazz->descriptor, desc->method->name,
             cUnit.numInsts);
    }

    /* Reset the compiler resource pool */
    dvmCompilerArenaReset();

    /*
     * Things have gone smoothly - publish the starting address of
     * translation's entry point.
     */
    if (!cUnit.halveInstCount) {
        return cUnit.baseAddr + cUnit.headerSize;

    /* Halve the instruction count and retry again */
    } else {
        return dvmCompileTrace(desc, cUnit.numInsts / 2);
    }
}

/*
 * Similar to dvmCompileTrace, but the entity processed here is the whole
 * method.
 *
 * TODO: implementation will be revisited when the trace builder can provide
 * whole-method traces.
 */
void *dvmCompileMethod(Method *method)
{
    const DexCode *dexCode = dvmGetMethodCode(method);
    const u2 *codePtr = dexCode->insns;
    const u2 *codeEnd = dexCode->insns + dexCode->insnsSize;
    int blockID = 0;
    unsigned int curOffset = 0;

    BasicBlock *firstBlock = dvmCompilerNewBB(DALVIK_BYTECODE);
    firstBlock->id = blockID++;

    /* Allocate the bit-vector to track the beginning of basic blocks */
    BitVector *bbStartAddr = dvmAllocBitVector(dexCode->insnsSize+1, false);
    dvmSetBit(bbStartAddr, 0);

    /*
     * Sequentially go through every instruction first and put them in a single
     * basic block. Identify block boundaries at the mean time.
     */
    while (codePtr < codeEnd) {
        MIR *insn = dvmCompilerNew(sizeof(MIR), false);
        insn->offset = curOffset;
        int width = parseInsn(codePtr, &insn->dalvikInsn, false);
        bool isInvoke = false;
        const Method *callee;
        insn->width = width;

        dvmCompilerAppendMIR(firstBlock, insn);
        /*
         * Check whether this is a block ending instruction and whether it
         * suggests the start of a new block
         */
        unsigned int target = curOffset;

        /*
         * If findBlockBoundary returns true, it means the current instruction
         * is terminating the current block. If it is a branch, the target
         * address will be recorded in target.
         */
        if (findBlockBoundary(method, insn, curOffset, &target, &isInvoke,
                              &callee)) {
            dvmSetBit(bbStartAddr, curOffset + width);
            if (target != curOffset) {
                dvmSetBit(bbStartAddr, target);
            }
        }

        codePtr += width;
        /* each bit represents 16-bit quantity */
        curOffset += width;
    }

    /*
     * The number of blocks will be equal to the number of bits set to 1 in the
     * bit vector minus 1, because the bit representing the location after the
     * last instruction is set to one.
     */
    int numBlocks = dvmCountSetBits(bbStartAddr);
    if (dvmIsBitSet(bbStartAddr, dexCode->insnsSize)) {
        numBlocks--;
    }

    CompilationUnit cUnit;
    BasicBlock **blockList;

    memset(&cUnit, 0, sizeof(CompilationUnit));
    cUnit.method = method;
    blockList = cUnit.blockList =
        dvmCompilerNew(sizeof(BasicBlock *) * numBlocks, true);

    /*
     * Register the first block onto the list and start split it into block
     * boundaries from there.
     */
    blockList[0] = firstBlock;
    cUnit.numBlocks = 1;

    int i;
    for (i = 0; i < numBlocks; i++) {
        MIR *insn;
        BasicBlock *curBB = blockList[i];
        curOffset = curBB->lastMIRInsn->offset;

        for (insn = curBB->firstMIRInsn->next; insn; insn = insn->next) {
            /* Found the beginning of a new block, see if it is created yet */
            if (dvmIsBitSet(bbStartAddr, insn->offset)) {
                int j;
                for (j = 0; j < cUnit.numBlocks; j++) {
                    if (blockList[j]->firstMIRInsn->offset == insn->offset)
                        break;
                }

                /* Block not split yet - do it now */
                if (j == cUnit.numBlocks) {
                    BasicBlock *newBB = dvmCompilerNewBB(DALVIK_BYTECODE);
                    newBB->id = blockID++;
                    newBB->firstMIRInsn = insn;
                    newBB->lastMIRInsn = curBB->lastMIRInsn;
                    curBB->lastMIRInsn = insn->prev;
                    insn->prev->next = NULL;
                    insn->prev = NULL;

                    /*
                     * If the insn is not an unconditional branch, set up the
                     * fallthrough link.
                     */
                    if (!isUnconditionalBranch(curBB->lastMIRInsn)) {
                        curBB->fallThrough = newBB;
                    }

                    /* enqueue the new block */
                    blockList[cUnit.numBlocks++] = newBB;
                    break;
                }
            }
        }
    }

    if (numBlocks != cUnit.numBlocks) {
        LOGE("Expect %d vs %d basic blocks\n", numBlocks, cUnit.numBlocks);
        dvmAbort();
    }

    dvmFreeBitVector(bbStartAddr);

    /* Connect the basic blocks through the taken links */
    for (i = 0; i < numBlocks; i++) {
        BasicBlock *curBB = blockList[i];
        MIR *insn = curBB->lastMIRInsn;
        unsigned int target = insn->offset;
        bool isInvoke;
        const Method *callee;

        findBlockBoundary(method, insn, target, &target, &isInvoke, &callee);

        /* Found a block ended on a branch */
        if (target != insn->offset) {
            int j;
            /* Forward branch */
            if (target > insn->offset) {
                j = i + 1;
            } else {
                /* Backward branch */
                j = 0;
            }
            for (; j < numBlocks; j++) {
                if (blockList[j]->firstMIRInsn->offset == target) {
                    curBB->taken = blockList[j];
                    break;
                }
            }

            if (j == numBlocks) {
                LOGE("Target not found for insn %x: expect target %x\n",
                     curBB->lastMIRInsn->offset, target);
                dvmAbort();
            }
        }
    }

    dvmCompilerMIR2LIR(&cUnit);

    dvmCompilerAssembleLIR(&cUnit);

    dvmCompilerDumpCompilationUnit(&cUnit);

    dvmCompilerArenaReset();

    return cUnit.baseAddr + cUnit.headerSize;
}
