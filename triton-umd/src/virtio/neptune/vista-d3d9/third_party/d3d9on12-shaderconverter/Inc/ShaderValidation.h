/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 * Bounded legacy token walking shared by the DDI and converter entry points.
 */
#pragma once
namespace ShaderConv {
inline unsigned LegacyOperandCount(unsigned opcode, unsigned version)
{
    const unsigned major=(version>>8)&255;
    const bool ps=(version>>16)==0xffff;
    switch(opcode) {
    case D3DSIO_NOP: case D3DSIO_RET: case D3DSIO_ENDLOOP:
    case D3DSIO_ENDREP: case D3DSIO_ELSE: case D3DSIO_ENDIF:
    case D3DSIO_BREAK: case D3DSIO_PHASE: case D3DSIO_END: return 0;
    case D3DSIO_CALL: case D3DSIO_LABEL: case D3DSIO_REP:
    case D3DSIO_IF: case D3DSIO_BREAKP: case D3DSIO_TEXKILL:
    case D3DSIO_TEXDEPTH: return 1;
    case D3DSIO_MOV: case D3DSIO_RCP: case D3DSIO_RSQ:
    case D3DSIO_EXP: case D3DSIO_LOG: case D3DSIO_LIT: case D3DSIO_FRC:
    case D3DSIO_ABS: case D3DSIO_NRM: case D3DSIO_MOVA:
    case D3DSIO_EXPP: case D3DSIO_LOGP: case D3DSIO_DSX: case D3DSIO_DSY:
    case D3DSIO_CALLNZ: case D3DSIO_LOOP: case D3DSIO_IFC:
    case D3DSIO_BREAKC: case D3DSIO_DCL: case D3DSIO_DEFB: return 2;
    case D3DSIO_ADD: case D3DSIO_SUB: case D3DSIO_MUL:
    case D3DSIO_DP3: case D3DSIO_DP4: case D3DSIO_MIN: case D3DSIO_MAX:
    case D3DSIO_SLT: case D3DSIO_SGE: case D3DSIO_DST:
    case D3DSIO_M4x4: case D3DSIO_M4x3: case D3DSIO_M3x4:
    case D3DSIO_M3x3: case D3DSIO_M3x2: case D3DSIO_POW: case D3DSIO_CRS:
    case D3DSIO_SETP: case D3DSIO_TEXLDL: case D3DSIO_BEM: return 3;
    case D3DSIO_MAD: case D3DSIO_LRP: case D3DSIO_CND:
    case D3DSIO_CMP: case D3DSIO_DP2ADD: return 4;
    case D3DSIO_SGN: case D3DSIO_SINCOS: return major>=3 ? 2 : 4;
    case D3DSIO_DEF: case D3DSIO_DEFI: case D3DSIO_TEXLDD: return 5;
    case D3DSIO_TEXCOORD: return ps && version<D3DPS_VERSION(1,4) ? 1 : 2;
    case D3DSIO_TEX: return major>=2 ? 3 : version==D3DPS_VERSION(1,4) ? 2 : 1;
    case D3DSIO_TEXBEM: case D3DSIO_TEXBEML: case D3DSIO_TEXREG2AR:
    case D3DSIO_TEXREG2GB: case D3DSIO_TEXREG2RGB: case D3DSIO_TEXM3x2PAD:
    case D3DSIO_TEXM3x2TEX: case D3DSIO_TEXM3x3PAD: case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXM3x3VSPEC: case D3DSIO_TEXDP3TEX: case D3DSIO_TEXDP3:
    case D3DSIO_TEXM3x2DEPTH: case D3DSIO_TEXM3x3: return 2;
    case D3DSIO_TEXM3x3SPEC: return 3;
    default: return ~0u;
    }
}
inline unsigned LegacyInstructionLength(unsigned token, unsigned version)
{
    unsigned op=token&D3DSI_OPCODE_MASK;
    if(op==D3DSIO_COMMENT) return ((token&D3DSI_COMMENTSIZE_MASK)>>D3DSI_COMMENTSIZE_SHIFT)+1;
    if(op==D3DSIO_DEF || op==D3DSIO_DEFI) return 6;
    if(op==D3DSIO_DEFB) return 3;
    if(((version>>8)&255)>=2) return ((token&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT)+1;
    unsigned operands=LegacyOperandCount(op,version);
    return operands==~0u ? 0 : operands+1;
}
/* Functions with the same call-graph rank cannot be active simultaneously.
 * Assign each rank a four-register loop bank, bounding temporary usage even
 * for thousands of sequential loops or independent subroutines. */
template <typename Word>
inline bool LegacyFunctionRanks(const Word *tokens, size_t words,
                                unsigned char (&ranks)[2049])
{
    if (!tokens || words<2) return false;
    size_t starts[2049], ends[2049];
    unsigned char visiting[2049] = {};
    for (unsigned i=0;i<2049;++i) { starts[i]=words; ends[i]=words-1; ranks[i]=0; }
    unsigned function=2048;
    starts[function]=1;
    for (size_t i=1;i+1<words;) {
        unsigned op=tokens[i]&D3DSI_OPCODE_MASK;
        unsigned length=LegacyInstructionLength(tokens[i],tokens[0]);
        if (!length || length>words-i) return false;
        if (op==D3DSIO_LABEL) {
            unsigned label=tokens[i+1]&D3DSP_REGNUM_MASK;
            if (label>=2048 || starts[label]!=words) return false;
            ends[function]=i; function=label; starts[function]=i+length;
        }
        i+=length;
    }
    auto rank=[&](auto &&self,unsigned id,unsigned depth)->bool {
        if (depth>4 || starts[id]==words || visiting[id]==1) return false;
        if (visiting[id]==2) return depth+ranks[id]<=4;
        visiting[id]=1;
        for (size_t i=starts[id];i<ends[id];) {
            unsigned op=tokens[i]&D3DSI_OPCODE_MASK;
            if (op==D3DSIO_CALL || op==D3DSIO_CALLNZ) {
                unsigned offset=(tokens[i]&D3DSHADER_INSTRUCTION_PREDICATED)?2:1;
                unsigned callee=tokens[i+offset]&D3DSP_REGNUM_MASK;
                if (callee>=2048 || !self(self,callee,depth+1)) return false;
                unsigned next=ranks[callee]+1;
                if (next>ranks[id]) ranks[id]=(unsigned char)next;
            }
            i+=LegacyInstructionLength(tokens[i],tokens[0]);
        }
        visiting[id]=2;
        return depth+ranks[id]<=4;
    };
    for (unsigned i=0;i<2049;++i)
        if (starts[i]!=words && !rank(rank,i,0)) return false;
    return true;
}
inline bool ValidateLegacyShader(const unsigned *tokens, size_t bytes, bool vertex,
                                 bool internalFixedFunction = false)
{
    if(!tokens || bytes<8 || (bytes&3) || bytes>(4u<<20)) return false;
    const unsigned version=tokens[0], major=(version>>8)&255, minor=version&255;
    if((version>>16)!=(vertex?0xfffeu:0xffffu)) return false;
    if(!((major==1 && (vertex ? minor==1 : minor>=1 && minor<=4)) ||
         (major==2 && minor<=1) || (major==3 && minor==0))) return false;
    const size_t words=bytes/4;
    unsigned blocks[64], depth=0, loopDepth=0;
    for(size_t i=1;i<words;) {
        unsigned token=tokens[i],op=token&D3DSI_OPCODE_MASK;
        if(op==D3DSIO_END) {
            if (token!=D3DSIO_END || i+1!=words || depth) return false;
            unsigned char ranks[2049];
            return LegacyFunctionRanks(tokens,words,ranks);
        }
        unsigned length=LegacyInstructionLength(token,version);
        if(!length || length>words-i) return false;
        if(op!=D3DSIO_COMMENT) {
            if (op==D3DSIO_LABEL && depth) return false;
            unsigned operands=LegacyOperandCount(op,version);
            if(operands==~0u || length<operands+1) return false;
            if(op==D3DSIO_IF || op==D3DSIO_IFC || op==D3DSIO_LOOP || op==D3DSIO_REP) {
                if(depth==64)return false;
                if ((op==D3DSIO_LOOP || op==D3DSIO_REP) && ++loopDepth>4) return false;
                blocks[depth++]=op;
            } else if(op==D3DSIO_ELSE) {
                if(!depth || (blocks[depth-1]!=D3DSIO_IF && blocks[depth-1]!=D3DSIO_IFC))return false;
                blocks[depth-1]=D3DSIO_ELSE;
            } else if(op==D3DSIO_ENDIF || op==D3DSIO_ENDLOOP || op==D3DSIO_ENDREP) {
                if(!depth)return false;unsigned start=blocks[--depth];
                if(op==D3DSIO_ENDIF ? start!=D3DSIO_IF && start!=D3DSIO_IFC && start!=D3DSIO_ELSE :
                   op==D3DSIO_ENDLOOP ? start!=D3DSIO_LOOP : start!=D3DSIO_REP)return false;
                if (op!=D3DSIO_ENDIF) --loopDepth;
            }
            if ((op==D3DSIO_BREAK || op==D3DSIO_BREAKC || op==D3DSIO_BREAKP) && !loopDepth) return false;
            size_t cursor=i+1;
            for(unsigned operand=0;operand<operands;++operand) {
                if(cursor>=i+length)return false;
                unsigned parameter=tokens[cursor++];
                if((op==D3DSIO_DCL && operand==0) ||
                   ((op==D3DSIO_DEF || op==D3DSIO_DEFI || op==D3DSIO_DEFB) && operand>0))continue;
                if(!(parameter&0x80000000u))return false;
                const unsigned type=((parameter&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT) |
                    ((parameter&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
                const unsigned reg=parameter&D3DSP_REGNUM_MASK;
                unsigned limit=0;
                switch(type) {
                case D3DSPR_TEMP: limit=32;break;
                case D3DSPR_INPUT: limit=vertex?16:10;break;
                case D3DSPR_CONST: limit=vertex ? (internalFixedFunction ? 1920 : 256) : 224;break;
                case D3DSPR_ADDR: limit=vertex?1:8;break;
                case D3DSPR_RASTOUT: limit=vertex && major<3 ? 3 : 0;break;
                case D3DSPR_ATTROUT: limit=vertex && major<3 ? 2 : 0;break;
                case D3DSPR_TEXCRDOUT: limit=vertex ? (major>=3 ? 12 : 8) : 0;break;
                case D3DSPR_CONSTINT: case D3DSPR_CONSTBOOL: limit=16;break;
                case D3DSPR_COLOROUT: limit=vertex ? 0 : 4;break;
                case D3DSPR_DEPTHOUT: limit=vertex ? 0 : 1;break;
                case D3DSPR_LOOP: case D3DSPR_PREDICATE: limit=1;break;
                case D3DSPR_SAMPLER: limit=vertex?4:16;break;
                case D3DSPR_MISCTYPE: limit=vertex ? 0 : 2;break;
                case D3DSPR_LABEL: limit=2048;break;
                default:return false;
                }
                if(reg>=limit)return false;
                if ((op==D3DSIO_CALL || op==D3DSIO_CALLNZ || op==D3DSIO_LABEL) &&
                    operand==0 && type!=D3DSPR_LABEL) return false;
                if (operand==2 && !(parameter&D3DSHADER_ADDRESSMODE_MASK)) {
                    unsigned span=op==D3DSIO_M4x4 || op==D3DSIO_M3x4 ? 4 :
                        op==D3DSIO_M4x3 || op==D3DSIO_M3x3 ? 3 : op==D3DSIO_M3x2 ? 2 : 1;
                    if (span>limit-reg) return false;
                }
                if((parameter&D3DSHADER_ADDRESSMODE_MASK) && major>=2) {
                    if(cursor>=i+length)return false;
                    unsigned address=tokens[cursor++];
                    unsigned addressType=((address&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT) |
                        ((address&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
                    if(!(address&0x80000000u) || (addressType!=D3DSPR_ADDR && addressType!=D3DSPR_LOOP) ||
                       (address&D3DSP_REGNUM_MASK))return false;
                }
                if (operand==0 && (token&D3DSHADER_INSTRUCTION_PREDICATED)) {
                    if (major<2 || cursor>=i+length || op==D3DSIO_DCL ||
                        op==D3DSIO_DEF || op==D3DSIO_DEFI || op==D3DSIO_DEFB ||
                        op==D3DSIO_CALL || op==D3DSIO_CALLNZ || op==D3DSIO_LABEL ||
                        op==D3DSIO_LOOP || op==D3DSIO_REP || op==D3DSIO_IF ||
                        op==D3DSIO_IFC || op==D3DSIO_BREAKC || op==D3DSIO_BREAKP ||
                        op==D3DSIO_TEXKILL) return false;
                    unsigned predicate=tokens[cursor++];
                    unsigned predicateType=((predicate&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT) |
                        ((predicate&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
                    if (!(predicate&0x80000000u) || predicateType!=D3DSPR_PREDICATE ||
                        (predicate&D3DSP_REGNUM_MASK) || (predicate&D3DSHADER_ADDRESSMODE_MASK)) return false;
                }
            }
            if(cursor!=i+length)return false;
        }
        i+=length;
    }
    return false;
}
}
