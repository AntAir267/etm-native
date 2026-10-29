#!/usr/bin/env python3
"""Direct3D 9 shader bytecode to SPIR-V, ahead of time, for our own Vulkan renderer (VULKAN.md
"Shaders"; option renderer = vulkan).

    d3d2spv.py table <game dir> <replacements dir> <captured dir> <out.c> [kinds file]   (the Makefile)
        every shader the game can give the device compiled into a C table keyed by the FNV-1a 64
        hash of its bytecode (runtime/vk_d3d9.c looks it up); rewritten only when it changes. With
        spirv-val on the PATH every module is validated (vulkan1.3). Enter the Matrix makes its
        shaders at run time with the D3DX it has linked in, from text (larush-native's game had
        them compiled in paks), so the sources are: the game's Shaders/*.vsh / *.psh assembled by
        tools/d3dasm.py [guess: the same bytes as D3DX assembles; no level of the test route makes
        them, VULKAN.md 1], the bytecode the game made in earlier runs (<captured dir>: the user
        folder's shaders/, written by ETM_D3D_INV and by our renderer for a shader without SPIR-V),
        and our replacements (data/shaders). Most of the game draws with Direct3D's fixed function
        (runtime/shaders/ff.vert, ff.frag: glslangValidator in the Makefile).
    d3d2spv.py one <file.vsh|.psh|raw bytecode> [-o out.spv]   one shader (spirv-dis to read it)
    d3d2spv.py check [game dir]        every shader compiled and validated, a summary printed

The game uses vs_2_0, ps_1_1 and ps_2_0; our passes vs_2_0, vs_3_0, ps_2_0 and ps_3_0. The meaning
of each instruction follows Direct3D 9 as DXVK 3.1.1's converter implements it (dxbc-spirv sm3/,
read as the reference for parity with the pictures it draws): "legacy" multiplies (0 * anything =
0, DXVK's strict float emulation on RADV and NVIDIA) in mul, mad, dp2add/dp3/dp4, the m* matrices,
lrp, pow, crs and nrm; rsq of |x|, log2 of |x|; ps_1_x constants clamped to [-1, 1]; vs_1-2 colour
outputs clamped to [0, 1]; ps_1_x co-issued pairs read their sources before either writes; table
fog (W fog: 1 / FragCoord.w) and the alpha test at the end of a pixel shader (fog not in ps_3_0);
vPos = FragCoord.xy - 0.5. The interface (must match runtime/vk_abi.h):
- vertex inputs at a location per usage and index (INPUT_LOC below); varyings: COLOR0-1 at 0-1,
  TEXCOORD0-7 at 2-9, FOG at 10 (scalar); every vertex shader writes all eleven (Direct3D's defaults
  where it does not: colour 0 (1,1,1,1), colour 1 (0,0,0,1), texture coordinates (0,0,0,1)? no:
  (0,0,0,0) as DXVK, fog 1) and PointSize 1;
- set 0 (bindless, bound once): binding 0 texture2D[], 1 textureCube[], 2 texture3D[], 3 texture2D
  depth[] (compare sampling), 4 sampler[]; a draw's push constants hold, per sampler slot 0-7, the
  texture's index (low 16 bits) and the sampler's (high 16);
- set 1: binding 0 the vertex shader's float constants as VS_CHUNKS uniform buffers of VS_CHUNK
  registers each (c[n] is chunk n / VS_CHUNK's register n % VS_CHUNK; each block is declared vec4
  c[256], so relative addressing reads through chunk 0, whose 256 registers the runtime uploads in
  one piece for such a shader: runtime/vk_d3d9.c "constants"), binding 1 the pixel shader's: vec4
  extra[8] (PS_EXTRA, fog, alpha test, bump matrices) then vec4 c[224]; dynamic offsets per draw;
- specialization constants: 0 alpha test (D3DCMP; 8 always = off), 1 fog (0 off, 1 the vertex
  shader's oFog, 2 exp, 3 exp2, 4 linear), 2 sampler kinds (2 bits a slot: 0 2D, 1 cube, 2 3D,
  3 2D depth compare), 3 projected slots (ps_1_x tex, D3DTTFF_PROJECTED), 4 the shadow filter
  (0, 3, 5, 7: DXVK's dxvk-soft-shadows.patch, option shadow_filter).
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3dasm  # noqa: E402

# ------------------------------------------------------------------ the interface (runtime/vk_abi.h)
USAGE_POSITION, USAGE_BLENDWEIGHT, USAGE_BLENDINDICES, USAGE_NORMAL, USAGE_PSIZE, USAGE_TEXCOORD, \
    USAGE_TANGENT, USAGE_BINORMAL, USAGE_TESSFACTOR, USAGE_POSITIONT, USAGE_COLOR, USAGE_FOG, \
    USAGE_DEPTH, USAGE_SAMPLE = range(14)


def INPUT_LOC(usage, index):
    """a vertex input's location by its declaration usage (runtime/vk_abi.h vk_input_loc)"""
    if usage in (USAGE_POSITION, USAGE_POSITIONT) and index == 0: return 0
    if usage == USAGE_BLENDWEIGHT and index == 0: return 1
    if usage == USAGE_BLENDINDICES and index == 0: return 2
    if usage == USAGE_NORMAL and index == 0: return 3
    if usage == USAGE_PSIZE and index == 0: return 4
    if usage == USAGE_TEXCOORD and index < 8: return 5 + index
    if usage == USAGE_TANGENT and index == 0: return 13
    if usage == USAGE_BINORMAL and index == 0: return 14
    if usage == USAGE_COLOR and index < 2: return 15 + index
    if usage == USAGE_FOG and index == 0: return 17
    if usage == USAGE_DEPTH and index == 0: return 18
    if usage == USAGE_POSITION and index == 1: return 19
    if usage == USAGE_NORMAL and index == 1: return 20
    raise CompileError('vertex input usage %d index %d has no location' % (usage, index))


VARY_COLOR, VARY_TEX, VARY_FOG, NVARY = 0, 2, 10, 11     # varying locations
VS_CONSTS, PS_CONSTS, PS_EXTRA = 256, 224, 8
VS_CHUNK, VS_CHUNKS = 32, 8             # (VK_VS_CHUNK, VK_VS_CHUNKS)
FC2_DEFAULT = 0x8 | 0x10000 | 0x20000 | 0x40000 | 0x4   # AllowRecip, AllowContract, AllowReassoc, AllowTransform, NSZ
SPEC_ALPHA, SPEC_FOG, SPEC_KINDS, SPEC_PROJ, SPEC_SHADOWF = range(5)
MAX_SAMPLERS = 8
# PS_EXTRA vec4s: 0 fog colour rgb, density; 1 fog end, fog scale, alpha reference, alpha scale (255:
# compare in 8 bits, 0: as floats); 2-5 bump matrices (stage 0-3: 00 01 10 11); 6-7 bump luminance
# scale and offset (stage 0-1 xy zw, 2-3)


class CompileError(Exception):
    pass


# ------------------------------------------------------------------ SPIR-V
OpName, OpExtension, OpExtInstImport, OpExtInst, OpMemoryModel, OpEntryPoint, OpExecutionMode, \
    OpCapability = 5, 10, 11, 12, 14, 15, 16, 17
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeImage, OpTypeSampler, \
    OpTypeSampledImage, OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer, OpTypeFunction = \
    19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 32, 33
OpConstantTrue, OpConstantFalse, OpConstant, OpConstantComposite, OpSpecConstant = 41, 42, 43, 44, 50
OpFunction, OpFunctionEnd, OpVariable, OpLoad, OpStore, OpAccessChain = 54, 56, 59, 61, 62, 65
OpDecorate, OpMemberDecorate = 71, 72
OpVectorShuffle, OpCompositeConstruct, OpCompositeExtract = 79, 80, 81
OpSampledImage, OpImageSampleImplicitLod, OpImageSampleExplicitLod, OpImageSampleDrefImplicitLod, \
    OpImageSampleDrefExplicitLod = 86, 87, 88, 89, 90
OpConvertFToS, OpConvertSToF, OpBitcast = 110, 111, 124
OpFNegate, OpIAdd, OpFAdd, OpISub, OpFSub, OpFMul, OpFDiv = 127, 128, 129, 130, 131, 133, 136
OpShiftRightLogical, OpBitwiseAnd = 194, 199
OpLogicalOr, OpLogicalAnd, OpLogicalNot, OpSelect, OpIEqual, OpINotEqual, OpULessThan, OpSLessThan = \
    166, 167, 168, 169, 170, 171, 176, 177
OpFOrdEqual, OpFOrdNotEqual, OpFOrdLessThan, OpFOrdGreaterThan, OpFOrdLessThanEqual, \
    OpFOrdGreaterThanEqual, OpFUnordNotEqual = 180, 182, 184, 186, 188, 190, 183
OpPhi, OpLoopMerge, OpSelectionMerge, OpLabel, OpBranch, OpBranchConditional, OpReturn = \
    245, 246, 247, 248, 249, 250, 253
OpDemoteToHelperInvocation = 5380
GLSL_RoundEven, GLSL_FAbs, GLSL_FSign, GLSL_Floor, GLSL_Fract, GLSL_Sin, GLSL_Cos, GLSL_Exp2, \
    GLSL_Log2, GLSL_InverseSqrt, GLSL_FMin, GLSL_FMax, GLSL_FClamp, GLSL_Fma = 2, 4, 6, 8, 10, 13, 14, 29, 30, 32, 37, 40, 43, 50
DEC_SpecId, DEC_Block, DEC_ArrayStride, DEC_BuiltIn, DEC_Flat, DEC_NonWritable, DEC_Location, \
    DEC_Binding, DEC_DescriptorSet, DEC_Offset, DEC_NoContraction, DEC_Centroid = 1, 2, 6, 11, 14, 24, 30, 33, 34, 35, 42, 16
SC_UniformConstant, SC_Input, SC_Uniform, SC_Output, SC_Function, SC_PushConstant = 0, 1, 2, 3, 7, 9


def spv_string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def fbits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


class Val:
    """an SSA value: its id, type id and vector size (1 for scalars)"""
    __slots__ = ('id', 'ty', 'n')

    def __init__(self, id_, ty, n):
        self.id, self.ty, self.n = id_, ty, n


class Module:
    def __init__(self):
        self.bound = 1
        self.caps, self.exts = [], []
        self.names, self.annot, self.glob = [], [], []
        self.types, self.consts = {}, {}
        self.vars = []                  # the function's OpVariables (first block)
        self.code = []                  # the function's body
        self.interface = []
        self.glsl = self.new()

    def new(self):
        i = self.bound
        self.bound += 1
        return i

    @staticmethod
    def ins(out, op, *a):
        out.append(((len(a) + 1) << 16) | op)
        out.extend(a)

    def op(self, op, rtype, *a):
        """an instruction with a result in the function body: its id"""
        r = self.new()
        self.ins(self.code, op, rtype, r, *a)
        return r

    def name(self, i, s):
        self.ins(self.names, OpName, i, *spv_string(s))

    def deco(self, i, d, *a):
        self.ins(self.annot, OpDecorate, i, d, *a)

    # types (deduplicated)
    def _t(self, key, op, *a):
        if key not in self.types:
            i = self.new()
            self.ins(self.glob, op, i, *a)
            self.types[key] = i
        return self.types[key]

    def t_void(self): return self._t('void', OpTypeVoid)
    def t_bool(self): return self._t('bool', OpTypeBool)
    def t_f32(self): return self._t('f32', OpTypeFloat, 32)
    def t_i32(self): return self._t('i32', OpTypeInt, 32, 1)
    def t_u32(self): return self._t('u32', OpTypeInt, 32, 0)

    def t_vec(self, base, n):
        if n == 1:
            return base
        return self._t(('vec', base, n), OpTypeVector, base, n)

    def t_fv(self, n): return self.t_vec(self.t_f32(), n)
    def t_bv(self, n): return self.t_vec(self.t_bool(), n)
    def t_ptr(self, sc, t): return self._t(('ptr', sc, t), OpTypePointer, sc, t)
    def t_func(self, r, *a): return self._t(('fn', r) + a, OpTypeFunction, r, *a)

    def t_image(self, dim, depth):
        # OpTypeImage sampled type, Dim (1 2D, 2 3D, 3 Cube), Depth, Arrayed, MS, Sampled 1, format Unknown
        return self._t(('img', dim, depth), OpTypeImage, self.t_f32(), dim, depth, 0, 0, 1, 0)

    def t_sampler(self): return self._t('sampler', OpTypeSampler)
    def t_sampled(self, img): return self._t(('simg', img), OpTypeSampledImage, img)
    def t_rtarray(self, t): return self._t(('rta', t), OpTypeRuntimeArray, t)

    def t_array(self, t, n, stride):
        k = ('arr', t, n)
        if k not in self.types:
            i = self._t(k, OpTypeArray, t, self.c_u32(n).id)
            self.deco(i, DEC_ArrayStride, stride)
        return self.types[k]

    # constants (deduplicated)
    def _c(self, key, t, op, *a):
        if key not in self.consts:
            i = self.new()
            self.ins(self.glob, op, t, i, *a)
            self.consts[key] = i
        return self.consts[key]

    def c_f32(self, f):
        b = fbits(f) if isinstance(f, float) or isinstance(f, int) else f
        return Val(self._c(('f', b), self.t_f32(), OpConstant, b), self.t_f32(), 1)

    def c_fbits(self, b):
        return Val(self._c(('f', b), self.t_f32(), OpConstant, b), self.t_f32(), 1)

    def c_u32(self, u): return Val(self._c(('u', u), self.t_u32(), OpConstant, u), self.t_u32(), 1)
    def c_i32(self, i): return Val(self._c(('i', i), self.t_i32(), OpConstant, i & 0xffffffff), self.t_i32(), 1)

    def c_bool(self, b):
        return Val(self._c(('b', b), self.t_bool(), OpConstantTrue if b else OpConstantFalse), self.t_bool(), 1)

    def c_vec(self, comps):
        """a float vector constant from floats or bit patterns ('b', bits)"""
        ids = []
        for c in comps:
            ids.append((self.c_fbits(c[1]) if isinstance(c, tuple) else self.c_f32(float(c))).id)
        n = len(ids)
        t = self.t_fv(n)
        return Val(self._c(('v', tuple(ids)), t, OpConstantComposite, *ids), t, n)

    def spec_u32(self, sid, default):
        i = self.new()
        self.ins(self.glob, OpSpecConstant, self.t_u32(), i, default)
        self.deco(i, DEC_SpecId, sid)
        return Val(i, self.t_u32(), 1)

    def gvar(self, ptr_t, sc, name=None, iface=True):
        i = self.new()
        self.ins(self.glob, OpVariable, ptr_t, i, sc)
        if name:
            self.name(i, name)
        if iface:
            self.interface.append(i)
        return i

    def fvar(self, t, name=None):
        i = self.new()
        self.ins(self.vars, OpVariable, self.t_ptr(SC_Function, t), i, SC_Function)
        if name:
            self.name(i, name)
        return i

    def label(self, i=None):
        i = i or self.new()
        self.ins(self.code, OpLabel, i)
        return i

    def words(self, model, entry_fn, exec_modes, version=0x00010600):
        out = [0x07230203, version, 0x4c615275, self.bound, 0]   # (generator: our own magic)
        for c in self.caps:
            self.ins(out, OpCapability, c)
        for e in self.exts:
            self.ins(out, OpExtension, *spv_string(e))
        self.ins(out, OpExtInstImport, self.glsl, *spv_string('GLSL.std.450'))
        self.ins(out, OpMemoryModel, 0, 1)            # Logical, GLSL450
        self.ins(out, OpEntryPoint, model, entry_fn, *(spv_string('main') + self.interface))
        for m in exec_modes:                          # ([OpExecutionModeId,] mode, operands...)
            if m[0] == 331:
                self.ins(out, 331, entry_fn, *m[1:])
            else:
                self.ins(out, OpExecutionMode, entry_fn, *m)
        return out + self.names + self.annot + self.glob


# ------------------------------------------------------------------ the bytecode, decoded
class Src:
    __slots__ = ('typ', 'num', 'swz', 'mod', 'rel')


class Dst:
    __slots__ = ('typ', 'num', 'mask', 'sat', 'shift', 'pp', 'centroid')


class Ins:
    __slots__ = ('op', 'ctrl', 'co', 'dst', 'src', 'dcl', 'raw', 'idx')


def decode(code):
    d = d3dasm.Dis(code)
    out = []
    for toks in d.split():
        t = toks[0]
        op = t & 0xffff
        if op == d3dasm.OP_COMMENT:
            continue
        i = Ins()
        i.op, i.ctrl, i.co = op, (t >> 16) & 0xff, bool(t & (1 << 30))
        i.dst, i.src, i.dcl, i.raw = None, [], None, toks[1:]
        if t & (1 << 28):
            raise CompileError('predicated instruction')
        p = toks[1:]
        if op == d3dasm.OP_DCL:
            i.dcl = p[0]
            i.dst = dst_of(p[1])
        elif op in (d3dasm.OP_DEF, d3dasm.OP_DEFI, d3dasm.OP_DEFB):
            i.dst = dst_of(p[0])
        else:
            if op not in d3dasm.OPS:
                raise CompileError('opcode %d' % op)
            k = 0
            if d3dasm.OPS[op][1] and p:
                i.dst = dst_of(p[0])
                k = 1
            while k < len(p):
                s, used = src_of(p, k, d.major)
                i.src.append(s)
                k += used
        out.append(i)
    return d, out


FUSE_CW = {'mov', 'add', 'sub', 'mul', 'mad', 'min', 'max', 'slt', 'sge', 'lrp', 'cmp', 'cnd', 'abs', 'frc', 'sgn'}
FUSE_SCALAR = {'rcp', 'rsq', 'exp', 'log', 'expp', 'logp', 'pow'}


def fusions(prog):
    """DXVK's multiply-add fusion (dxbc-spirv ArithmeticPass fuseMad, after scalarising): an add or sub
    one of whose operands is a multiply's result used nowhere else becomes one Fma (c - a x b as
    fma(-a, b, c), a x b - c as fma(a, b, -c); a negated or otherwise modified operand is not a
    multiply). In straight-line code, per component: {(add index, source, component): (mul index,
    its component)}."""
    last = {}                   # (register type, number, component) -> (mul index, component) or None
    uses = {}                   # (mul index, component) -> [use] (use: (add index, source, component) or None)
    groups = []                 # (a ps_1_x co-issued pair reads before either writes)
    for i in prog:
        if i.co and groups:
            groups[-1].append(i)
        else:
            groups.append([i])
    for g in groups:
      writes = []
      for i in g:
        name = d3dasm.OPS[i.op][0]
        reads = []              # ((type, num, comp), use)
        mask = i.dst.mask if i.dst is not None else 15
        for si, src in enumerate(i.src):
            if src.typ not in (R_TEMP, R_ADDR_TEX) or src.rel is not None:
                continue
            if name in FUSE_CW:
                for k in range(4):
                    if mask >> k & 1:
                        ok = name in ('add', 'sub') and src.mod in (0, 1) and not (name == 'add' and src.mod == 1)
                        if name == 'sub' and src.mod == 1:
                            ok = False
                        reads.append(((src.typ, src.num, src.swz[k]), (i.idx, si, k) if ok else None))
            elif name in FUSE_SCALAR:
                reads.append(((src.typ, src.num, src.swz[0]), None))
            elif name in ('dp3', 'dp4', 'dp2add'):
                n = {'dp3': 3, 'dp4': 4, 'dp2add': 2}[name] if si < 2 else 1
                for k in range(n):
                    reads.append(((src.typ, src.num, src.swz[k]), None))
            else:
                for k in range(4):
                    reads.append(((src.typ, src.num, k), None))
        for key, use in reads:
            w = last.get(key)
            if w is not None:
                uses.setdefault(w, []).append(use)
        if i.dst is not None and i.dst.typ in (R_TEMP, R_ADDR_TEX, R_TEXOUT, R_ATTROUT, R_RASTOUT, R_COLOROUT):
            plain = name == 'mul' and not i.dst.sat and not i.dst.shift
            for k in range(4):
                if mask >> k & 1:
                    writes.append(((i.dst.typ, i.dst.num, k), (i.idx, k) if plain else None))
      for key, w in writes:
          last[key] = w
    out = {}
    for w, us in uses.items():
        if len(us) == 1 and us[0] is not None:
            out[us[0]] = w
    # (an add whose both operands fuse: the first, as DXVK)
    seen = {}
    for (j, si, k) in sorted(out):
        if (j, k) in seen:
            del out[(j, si, k)]
        else:
            seen[(j, k)] = si
    return out


def dst_of(t):
    x = Dst()
    x.typ, x.num = d3dasm.tok_reg(t)
    x.mask = (t >> 16) & 15
    rm = (t >> 20) & 15
    x.sat, x.pp, x.centroid = bool(rm & 1), bool(rm & 2), bool(rm & 4)
    sh = (t >> 24) & 15
    x.shift = sh - 16 if sh >= 8 else sh
    if t & (1 << 13):
        raise CompileError('relative destination')
    return x


def src_of(p, k, major):
    t = p[k]
    x = Src()
    x.typ, x.num = d3dasm.tok_reg(t)
    s = (t >> 16) & 0xff
    x.swz = [(s >> (2 * c)) & 3 for c in range(4)]
    x.mod = (t >> 24) & 15
    x.rel = None
    used = 1
    if t & (1 << 13):
        if major >= 2:
            r = p[k + 1]
            used = 2
            rt, rn = d3dasm.tok_reg(r)
            x.rel = (rt, rn, (r >> 16) & 3)
        else:
            x.rel = (3, 0, 0)               # a0.x
    return x, used


# register types
R_TEMP, R_INPUT, R_CONST, R_ADDR_TEX, R_RASTOUT, R_ATTROUT, R_TEXOUT, R_CONSTINT, R_COLOROUT, \
    R_DEPTHOUT, R_SAMPLER, R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPF16, R_MISC, \
    R_LABEL, R_PRED = range(20)


# ------------------------------------------------------------------ the compiler
class Shader:
    def __init__(self, code, name='', narrow=None):
        self.name = name
        self.narrow = narrow                        # (kinds by slot, filters) seen in use: see NARROW
        self.dis, self.prog = decode(code)
        self.ps, self.major, self.minor = self.dis.ps, self.dis.major, self.dis.minor
        self.m = Module()
        m = self.m
        m.caps += [1]                               # Shader
        if self.ps:
            m.caps += [5302, 50, 5379]              # RuntimeDescriptorArray, ImageQuery, DemoteToHelperInvocation
        self.regs = {}                              # (type, num) -> Function variable (vec4)
        self.defs = {}                              # (type, num) -> constant Val (def/defi/defb)
        self.cmax = 0                               # constants read: highest + 1
        self.rel = False
        self.inputs = {}                            # vs: location -> (usage, index); ps: var
        self.in_vars = {}
        self.out_regs = {}                          # vs outputs: (type, num) -> (function var, location/builtin)
        self.dcl_usage = {}                         # sm3: (type, num) -> (usage, index)
        self.centroid_in = set()                    # pixel shader inputs declared _centroid
        self.samplers = {}                          # sampler slot -> dim ('2d', 'cube', 'volume', None: any (ps_1_x))
        self.proj_used = 0
        self.writes_depth = False
        self.kills = False
        self.defs_list = []                         # (register, 4 float bits) for the runtime
        self.build()

    # ---------------------------------------------------------- values
    def fv(self, n): return self.m.t_fv(n)

    def lab(self, i):
        self.m.label(i)
        self.cur_label = i

    def splat(self, v, n=4):
        if v.n == n:
            return v
        if v.n != 1:
            v = self.extract(v, 0)
        return Val(self.m.op(OpCompositeConstruct, self.fv(n), *([v.id] * n)), self.fv(n), n)

    def extract(self, v, c):
        if v.n == 1:
            return v
        return Val(self.m.op(OpCompositeExtract, self.m.t_f32(), v.id, c), self.m.t_f32(), 1)

    def shuffle(self, v, comps):
        if v.n == 1:
            return self.splat(v, len(comps)) if len(comps) > 1 else v
        if len(comps) == 1:
            return self.extract(v, comps[0])
        if comps == list(range(v.n)) and len(comps) == v.n:
            return v
        return Val(self.m.op(OpVectorShuffle, self.fv(len(comps)), v.id, v.id, *comps), self.fv(len(comps)), len(comps))

    def construct(self, comps):
        if len(comps) == 1:
            return comps[0]
        return Val(self.m.op(OpCompositeConstruct, self.fv(len(comps)), *[c.id for c in comps]), self.fv(len(comps)), len(comps))

    def const(self, *fs):
        if len(fs) == 1:
            return self.m.c_f32(float(fs[0]))
        return self.m.c_vec(list(fs))

    def binop(self, op, a, b, precise=False):
        """a float add, subtract, multiply or divide. DXVK's rounding (VULKAN.md 3.1): its sm3 shaders let
        the driver fuse and reorder plain arithmetic (float_controls2's default mode with AllowContract,
        AllowReassoc, AllowRecip, AllowTransform where it uses it, no mode at all elsewhere), fuse mad and
        the dot products themselves (Fma), and keep the dot products' chains exact (precise: here a
        NoContraction decoration, which the runtime turns into FPFastMathMode 0 under float_controls2;
        module_of / variant below)"""
        n = max(a.n, b.n)
        a, b = self.splat(a, n), self.splat(b, n)
        r = self.m.op(op, self.fv(n), a.id, b.id)
        if precise:
            self.m.deco(r, DEC_NoContraction)
        return Val(r, self.fv(n), n)

    def add(self, a, b): return self.binop(OpFAdd, a, b)
    def sub(self, a, b): return self.binop(OpFSub, a, b)
    def fmul(self, a, b): return self.binop(OpFMul, a, b)
    def div(self, a, b): return self.binop(OpFDiv, a, b)

    def neg(self, a):
        return Val(self.m.op(OpFNegate, a.ty, a.id), a.ty, a.n)

    def ext(self, inst, *args):
        a0 = args[0]
        n = max(a.n for a in args)
        args = [self.splat(a, n) for a in args]
        return Val(self.m.op(OpExtInst, self.fv(n), self.m.glsl, inst, *[a.id for a in args]), self.fv(n), n)

    def cmp(self, op, a, b):
        n = max(a.n, b.n)
        a, b = self.splat(a, n), self.splat(b, n)
        return Val(self.m.op(op, self.m.t_bv(n), a.id, b.id), self.m.t_bv(n), n)

    def select(self, c, a, b):
        n = max(a.n, b.n, c.n)
        a, b = self.splat(a, n), self.splat(b, n)
        return Val(self.m.op(OpSelect, self.fv(n), c.id, a.id, b.id), self.fv(n), n)

    def legacy(self, a, b):
        """Direct3D 9's multiply's operands: 0 times anything (inf, NaN) is 0 (DXVK's strict float
        emulation: (b == 0 ? 0 : a) x (a == 0 ? 0 : b))"""
        n = max(a.n, b.n)
        a, b = self.splat(a, n), self.splat(b, n)
        z = self.const(*([0.0] * n)) if n > 1 else self.const(0.0)
        a2 = self.select(self.cmp(OpFOrdEqual, b, z), z, a)
        b2 = self.select(self.cmp(OpFOrdEqual, a, z), z, b)
        return a2, b2

    def mul(self, a, b, precise=False):
        a2, b2 = self.legacy(a, b)
        return self.binop(OpFMul, a2, b2, precise)

    def mad(self, a, b, c, precise=False):
        """a x b + c fused (one rounding), as DXVK's mad"""
        a2, b2 = self.legacy(a, b)
        r = self.ext(GLSL_Fma, a2, b2, self.splat(c, a2.n))
        if precise:
            self.m.deco(r.id, DEC_NoContraction)
        return r

    def dot(self, a, b, n, acc=None):
        """as DXVK's dot products: the first product, then each next one fused onto it in order (dp2add:
        both fused onto the addend), the chain precise"""
        r = acc
        for i in range(n):
            x, y = self.extract(a, i), self.extract(b, i)
            r = self.mul(x, y, True) if r is None else self.mad(x, y, r, True)
        return r

    def saturate(self, v):
        n = v.n
        return self.ext(GLSL_FClamp, v, self.splat(self.const(0.0), n) if n > 1 else self.const(0.0),
                        self.splat(self.const(1.0), n) if n > 1 else self.const(1.0))

    def clamp(self, v, lo, hi):
        n = v.n
        return self.ext(GLSL_FClamp, v, self.splat(self.const(lo), n), self.splat(self.const(hi), n))

    # ---------------------------------------------------------- registers
    def reg(self, typ, num, name=None):
        k = (typ, num)
        if k not in self.regs:
            self.regs[k] = self.m.fvar(self.fv(4), name or 'r%d_%d' % k)
        return self.regs[k]

    def load_reg(self, typ, num):
        v = self.reg(typ, num)
        return Val(self.m.op(OpLoad, self.fv(4), v), self.fv(4), 4)

    def store_reg(self, var, val, mask):
        """the components of mask from val (a vec4) into the vec4 variable"""
        if mask == 15:
            self.m.ins(self.m.code, OpStore, var, val.id)
            return
        old = self.m.op(OpLoad, self.fv(4), var)
        comps = [4 + c if mask >> c & 1 else c for c in range(4)]
        new = self.m.op(OpVectorShuffle, self.fv(4), old, val.id, *comps)
        self.m.ins(self.m.code, OpStore, var, new)

    def const_load(self, num, rel):
        """a float constant: def'd ones inline, the rest from the uniform buffer"""
        if rel is None and (R_CONST, num) in self.defs:
            return self.defs[(R_CONST, num)]
        m = self.m
        idx = m.c_i32(num + (PS_EXTRA if self.ps else 0))
        chunk = m.c_i32(0)
        if rel is None:
            self.cmax = max(self.cmax, num + 1)
            index = idx
            if not self.ps:
                chunk, index = m.c_i32(num // VS_CHUNK), m.c_i32(num % VS_CHUNK)
        else:
            self.rel = True
            self.cmax = VS_CONSTS if not self.ps else PS_CONSTS
            a = self.load_addr(rel)
            index = Val(m.op(OpIAdd, m.t_i32(), a.id, idx.id), m.t_i32(), 1)
            top = (PS_EXTRA + PS_CONSTS if self.ps else VS_CONSTS) - 1
            index = Val(m.op(OpExtInst, m.t_i32(), m.glsl, 45, index.id, m.c_i32(0).id, m.c_i32(top).id),
                        m.t_i32(), 1)           # SClamp: out of range reads stay in the buffer [guess: unused]
        if self.ps:
            ptr = m.op(OpAccessChain, m.t_ptr(SC_Uniform, self.fv(4)), self.cbuf, m.c_i32(0).id, index.id)
        else:
            ptr = m.op(OpAccessChain, m.t_ptr(SC_Uniform, self.fv(4)), self.cbuf, chunk.id, m.c_i32(0).id, index.id)
        v = Val(m.op(OpLoad, self.fv(4), ptr), self.fv(4), 4)
        if rel is not None:
            # (out of range reads 0; the index is clamped so the access stays in the buffer)
            pass
        if self.ps and self.major == 1:
            v = self.clamp(v, -1.0, 1.0)
        return v

    def load_addr(self, rel):
        rt, rn, comp = rel
        m = self.m
        if rt == R_LOOP:
            raise CompileError('aL relative addressing')
        var = self.addr_var()
        p = m.op(OpAccessChain, m.t_ptr(SC_Function, m.t_i32()), var, m.c_i32(comp).id)
        v = Val(m.op(OpLoad, m.t_i32(), p), m.t_i32(), 1)
        # clamp the index into the buffer: Direct3D reads 0 past the end [guess: never reached]
        return v

    def addr_var(self):
        if not hasattr(self, '_a0'):
            self._a0 = self.m.fvar(self.m.t_vec(self.m.t_i32(), 4), 'a0')
        return self._a0

    def src_raw(self, s):
        """the register's vec4, before swizzle and modifiers"""
        t = s.typ
        if t == R_CONST:
            return self.const_load(s.num, s.rel)
        if t in (R_CONST2, R_CONST3, R_CONST4):
            return self.const_load(s.num + {R_CONST2: 2048, R_CONST3: 4096, R_CONST4: 6144}[t], s.rel)
        if t in (R_CONSTINT, R_CONSTBOOL):
            if (t, s.num) not in self.defs:
                raise CompileError('integer/bool constant i/b%d without def' % s.num)
            return self.defs[(t, s.num)]
        if t == R_INPUT and not self.ps:
            return self.vs_input(s.num)
        if t == R_INPUT and self.ps:
            return self.ps_input(R_INPUT, s.num)
        if t == R_ADDR_TEX and self.ps:
            if self.major == 1:
                return self.load_reg(R_ADDR_TEX, s.num)     # ps_1_x texture register (a temp)
            return self.ps_input(R_ADDR_TEX, s.num)
        if t == R_MISC and self.ps:
            return self.ps_misc(s.num)
        if t == R_TEMP:
            return self.load_reg(R_TEMP, s.num)
        if t == R_ADDR_TEX and not self.ps:
            raise CompileError('a0 as a source')
        raise CompileError('source register type %d' % t)

    def src(self, s, n=4):
        """a source's first n components after swizzle and modifiers"""
        v = self.src_raw(s)
        if s.mod in (9, 10):
            raise CompileError('_dz/_dw')
        v = self.shuffle(v, s.swz[:n]) if n < 4 or s.swz != [0, 1, 2, 3] else v
        mod = s.mod
        if mod in (11, 12):                       # abs, -abs
            v = self.ext(GLSL_FAbs, v)
            if mod == 12:
                v = self.neg(v)
        elif mod == 1:
            v = self.neg(v)
        elif mod == 2:
            v = self.sub(v, self.const(0.5))
        elif mod == 3:
            v = self.sub(self.const(0.5), v)
        elif mod == 4:
            v = self.add(self.fmul(v, self.const(2.0)), self.const(-1.0))
        elif mod == 5:
            v = self.add(self.fmul(v, self.const(-2.0)), self.const(1.0))
        elif mod == 6:
            v = self.sub(self.const(1.0), v)
        elif mod == 7:
            v = self.fmul(v, self.const(2.0))
        elif mod == 8:
            v = self.fmul(v, self.const(-2.0))
        elif mod == 13:
            raise CompileError('! on a bool')
        elif mod != 0:
            raise CompileError('source modifier %d' % mod)
        return v

    def dst_store(self, d, v):
        """v (vec4, or a scalar broadcast) through the destination's shift and saturate into its register"""
        v = self.splat(v, 4)
        if d.shift:
            v = self.fmul(v, self.const(float(2 ** d.shift)))
        if d.sat:
            v = self.saturate(v)
        self.write(d, v)

    def write(self, d, v):
        t = d.typ
        if t == R_TEMP:
            self.store_reg(self.reg(R_TEMP, d.num), v, d.mask)
        elif t == R_ADDR_TEX and self.ps and self.major == 1:
            self.store_reg(self.reg(R_ADDR_TEX, d.num), v, d.mask)
        elif t == R_ADDR_TEX and not self.ps:
            raise CompileError('a0 written by %s' % 'mov')
        elif t in (R_RASTOUT, R_ATTROUT, R_TEXOUT) and not self.ps:
            self.store_reg(self.vs_out(t, d.num), v, d.mask)
        elif t == R_COLOROUT and self.ps:
            if d.num != 0:
                raise CompileError('oC%d' % d.num)
            self.store_reg(self.reg(R_COLOROUT, 0, 'oC0'), v, d.mask)
        elif t == R_DEPTHOUT and self.ps:
            self.writes_depth = True
            self.store_reg(self.reg(R_DEPTHOUT, 0, 'oDepth'), v, d.mask)
        else:
            raise CompileError('destination register type %d' % t)

    # ---------------------------------------------------------- inputs and outputs
    def vs_input(self, num):
        k = (R_INPUT, num)
        if k not in self.in_vars:
            if k not in self.dcl_usage:
                raise CompileError('vertex input v%d not declared' % num)
            usage, index = self.dcl_usage[k]
            loc = INPUT_LOC(usage, index)
            var = self.m.gvar(self.m.t_ptr(SC_Input, self.fv(4)), SC_Input, 'v%d' % num)
            self.m.deco(var, DEC_Location, loc)
            self.inputs[loc] = (usage, index)
            self.in_vars[k] = var
        return Val(self.m.op(OpLoad, self.fv(4), self.in_vars[k]), self.fv(4), 4)

    def vary_loc(self, usage, index):
        if usage == USAGE_COLOR and index < 2: return VARY_COLOR + index
        if usage == USAGE_TEXCOORD and index < 8: return VARY_TEX + index
        if usage == USAGE_FOG and index == 0: return VARY_FOG
        raise CompileError('varying usage %d index %d' % (usage, index))

    def ps_input(self, typ, num):
        """a pixel shader input (colour v, texture coordinate t; sm3: v by its dcl): its value"""
        k = ('in', typ, num)
        if k not in self.in_vars:
            if self.major >= 3:
                if (typ, num) not in self.dcl_usage:
                    raise CompileError('input v%d not declared' % num)
                loc = self.vary_loc(*self.dcl_usage[(typ, num)])
            else:
                loc = (VARY_COLOR if typ == R_INPUT else VARY_TEX) + num
            var = self.m.gvar(self.m.t_ptr(SC_Input, self.fv(4)), SC_Input, '%s%d' % ('v' if typ == R_INPUT else 't', num))
            self.m.deco(var, DEC_Location, loc)
            # colours at the centroid of the covered samples, the rest at the pixel's centre unless
            # declared _centroid (as DXVK's sm3 io map; multisampled targets: the triangles' edge pixels)
            colour = loc in (VARY_COLOR, VARY_COLOR + 1)
            if colour or (typ, num) in self.centroid_in:
                self.m.deco(var, DEC_Centroid)
            self.in_vars[k] = var
        return Val(self.m.op(OpLoad, self.fv(4), self.in_vars[k]), self.fv(4), 4)

    def texcoord_in(self, num):
        return self.ps_input(R_ADDR_TEX, num)

    def frag_coord(self):
        if not hasattr(self, '_fc'):
            self._fc = self.m.gvar(self.m.t_ptr(SC_Input, self.fv(4)), SC_Input, 'FragCoord')
            self.m.deco(self._fc, DEC_BuiltIn, 15)
        return Val(self.m.op(OpLoad, self.fv(4), self._fc), self.fv(4), 4)

    def ps_misc(self, num):
        if num == 0:                               # vPos: the pixel's centre at integer coordinates
            fc = self.frag_coord()
            xy = self.sub(self.shuffle(fc, [0, 1]), self.const(0.5))
            return self.construct([self.extract(xy, 0), self.extract(xy, 1), self.const(0.0), self.const(1.0)])
        raise CompileError('vFace')

    def vs_out(self, typ, num):
        k = (typ, num)
        if k not in self.out_regs:
            if self.major >= 3:
                if (typ, num) not in self.dcl_usage:
                    raise CompileError('output o%d not declared' % num)
                usage, index = self.dcl_usage[(typ, num)]
                if usage == USAGE_POSITION:
                    tgt = 'pos'
                elif usage == USAGE_PSIZE:
                    tgt = 'psize'
                else:
                    tgt = self.vary_loc(usage, index)
            elif typ == R_RASTOUT:
                tgt = ['pos', VARY_FOG, 'psize'][num]
            elif typ == R_ATTROUT:
                tgt = VARY_COLOR + num
            else:
                tgt = VARY_TEX + num
            var = self.m.fvar(self.fv(4), 'o%d_%d' % k)
            self.out_regs[k] = (var, tgt)
        return self.out_regs[k][0]

    # ---------------------------------------------------------- textures
    def tex_bindings(self):
        if hasattr(self, '_tex'):
            return
        m = self.m
        self._tex = {}
        for b, (dim, depth) in enumerate([(1, 0), (3, 0), (2, 0), (1, 1)]):
            img = m.t_image(dim, depth)
            var = m.gvar(m.t_ptr(SC_UniformConstant, m.t_rtarray(img)), SC_UniformConstant,
                         ['tex2D', 'texCube', 'tex3D', 'texShadow'][b])
            m.deco(var, DEC_DescriptorSet, 0)
            m.deco(var, DEC_Binding, b)
            self._tex[b] = (var, img)
        smp = m.gvar(m.t_ptr(SC_UniformConstant, m.t_rtarray(m.t_sampler())), SC_UniformConstant, 'samplers')
        m.deco(smp, DEC_DescriptorSet, 0)
        m.deco(smp, DEC_Binding, 4)
        self._smp = smp
        # push constants: uint slots[8]
        arr = m.t_array(m.t_u32(), MAX_SAMPLERS, 4)
        st = m.new()
        m.ins(m.glob, OpTypeStruct, st, arr)
        m.deco(st, DEC_Block)
        m.ins(m.annot, OpMemberDecorate, st, 0, DEC_Offset, 0)
        self._pc = m.gvar(m.t_ptr(SC_PushConstant, st), SC_PushConstant, 'slots')
        self.spec_kinds = m.spec_u32(SPEC_KINDS, 0)
        self.spec_proj = m.spec_u32(SPEC_PROJ, 0)

    def spec_shadowf(self):
        if not hasattr(self, '_shadowf'):
            self._shadowf = self.m.spec_u32(SPEC_SHADOWF, 0)
        return self._shadowf

    def slot_word(self, s):
        m = self.m
        p = m.op(OpAccessChain, m.t_ptr(SC_PushConstant, m.t_u32()), self._pc, m.c_i32(0).id, m.c_i32(s).id)
        return Val(m.op(OpLoad, m.t_u32(), p), m.t_u32(), 1)

    def spec_field(self, spec, shift, bits):
        m = self.m
        v = m.op(OpShiftRightLogical, m.t_u32(), spec.id, m.c_u32(shift).id)
        return Val(m.op(OpBitwiseAnd, m.t_u32(), v, m.c_u32((1 << bits) - 1).id), m.t_u32(), 1)

    def branch(self, cases):
        """structured if/else chain: cases = [(bool Val or None for the last, fn() -> vec4 Val)]; the
        chosen branch's value"""
        m = self.m
        merge = m.new()
        results = []
        cur_cases = list(cases)
        # nested: if c0 then f0 else (if c1 then f1 else ...)
        def emit(cs, merge_id):
            cond, fn = cs[0]
            if cond is None or len(cs) == 1:
                v = fn()
                return v
            lt, lf = m.new(), m.new()
            inner_merge = m.new()
            m.ins(m.code, OpSelectionMerge, inner_merge, 0)
            m.ins(m.code, OpBranchConditional, cond.id, lt, lf)
            self.lab(lt)
            v1 = fn()
            end1 = self.cur_label
            m.ins(m.code, OpBranch, inner_merge)
            self.lab(lf)
            v2 = emit(cs[1:], inner_merge)
            end2 = self.cur_label
            m.ins(m.code, OpBranch, inner_merge)
            self.lab(inner_merge)
            r = m.op(OpPhi, self.fv(4), v1.id, end1, v2.id, end2)
            return Val(r, self.fv(4), 4)
        return emit(cur_cases, merge)

    def sample(self, s, coord, kind_fixed=None, lod=None, bias=None):
        """sampler slot s at coord (vec4): vec4. kind_fixed: '2d' (2D or depth compare by the spec
        constant), 'cube', 'volume', or None (any: ps_1_x, by the spec constant)"""
        if s >= MAX_SAMPLERS:
            raise CompileError('sampler s%d (at most %d)' % (s, MAX_SAMPLERS))
        self.tex_bindings()
        m = self.m
        word = self.slot_word(s)
        tidx = Val(m.op(OpBitwiseAnd, m.t_u32(), word.id, m.c_u32(0xffff).id), m.t_u32(), 1)
        sidx = Val(m.op(OpShiftRightLogical, m.t_u32(), word.id, m.c_u32(16).id), m.t_u32(), 1)
        kind = self.spec_field(self.spec_kinds, 2 * s, 2)

        def color(b, ncoord):
            def f():
                var, img = self._tex[b]
                p = m.op(OpAccessChain, m.t_ptr(SC_UniformConstant, img), var, tidx.id)
                im = m.op(OpLoad, img, p)
                sp = m.op(OpAccessChain, m.t_ptr(SC_UniformConstant, m.t_sampler()), self._smp, sidx.id)
                sm = m.op(OpLoad, m.t_sampler(), sp)
                si = m.op(OpSampledImage, m.t_sampled(img), im, sm)
                c = self.shuffle(coord, list(range(ncoord)))
                if lod is not None:
                    r = m.op(OpImageSampleExplicitLod, self.fv(4), si, c.id, 2, lod.id)     # Lod
                elif bias is not None:
                    r = m.op(OpImageSampleImplicitLod, self.fv(4), si, c.id, 1, bias.id)    # Bias
                else:
                    r = m.op(OpImageSampleImplicitLod, self.fv(4), si, c.id)
                return Val(r, self.fv(4), 4)
            return f

        def shadow():
            var, img = self._tex[3]
            c = self.shuffle(coord, [0, 1])
            dref = self.extract(coord, 2)

            def load():
                """(image, sampled image) in the current block: a sampled image is used in its own block"""
                p = m.op(OpAccessChain, m.t_ptr(SC_UniformConstant, img), var, tidx.id)
                im = m.op(OpLoad, img, p)
                sp = m.op(OpAccessChain, m.t_ptr(SC_UniformConstant, m.t_sampler()), self._smp, sidx.id)
                sm = m.op(OpLoad, m.t_sampler(), sp)
                return im, m.op(OpSampledImage, m.t_sampled(img), im, sm)

            def single():
                im, si = load()
                if lod is not None:
                    r = m.op(OpImageSampleDrefExplicitLod, m.t_f32(), si, c.id, dref.id, 2, lod.id)
                else:
                    r = m.op(OpImageSampleDrefImplicitLod, m.t_f32(), si, c.id, dref.id)
                return self.splat(Val(r, m.t_f32(), 1), 4)
            if not self.ps:
                return single()
            # the shadow filter (spec 4: 3, 5 or 7 texels square; option shadow_filter): DXVK's
            # dxvk-soft-shadows.patch, smooth percentage-closer filtering from bilinear comparison
            # samples (I. Castano's): weights and positions per axis from the fraction past the
            # nearest texel corner, explicit LOD 0
            filt = self.spec_shadowf()

            def filtered(n):
                def f():
                    im, si = load()
                    isz = m.op(103, m.t_vec(m.t_i32(), 2), im, m.c_i32(0).id)          # OpImageQuerySizeLod
                    fsz = Val(m.op(OpConvertSToF, self.fv(2), isz), self.fv(2), 2)
                    C = self.const
                    axes = []
                    for i in range(2):
                        size = self.extract(fsz, i)
                        t = self.fmul(self.extract(c, i), size)
                        corner = self.ext(GLSL_Floor, self.add(t, C(0.5)))
                        sf = self.sub(self.add(t, C(0.5)), corner)
                        base = self.sub(corner, C(0.5))
                        if n == 3:
                            w = [self.sub(C(3.0), self.fmul(C(2.0), sf)), self.add(C(1.0), self.fmul(C(2.0), sf))]
                            u = [self.sub(self.div(self.sub(C(2.0), sf), w[0]), C(1.0)), self.add(self.div(sf, w[1]), C(1.0))]
                        elif n == 5:
                            w = [self.sub(C(4.0), self.fmul(C(3.0), sf)), C(7.0), self.add(C(1.0), self.fmul(C(3.0), sf))]
                            u = [self.sub(self.div(self.sub(C(3.0), self.fmul(C(2.0), sf)), w[0]), C(2.0)),
                                 self.div(self.add(C(3.0), sf), C(7.0)), self.add(self.div(sf, w[2]), C(2.0))]
                        else:
                            w = [self.sub(self.fmul(C(5.0), sf), C(6.0)), self.sub(self.fmul(C(11.0), sf), C(28.0)),
                                 self.sub(self.fmul(C(-11.0), sf), C(17.0)), self.sub(self.fmul(C(-5.0), sf), C(1.0))]
                            u = [self.sub(self.div(self.sub(self.fmul(C(4.0), sf), C(5.0)), w[0]), C(3.0)),
                                 self.sub(self.div(self.sub(self.fmul(C(4.0), sf), C(16.0)), w[1]), C(1.0)),
                                 self.add(self.div(self.sub(self.fmul(C(-7.0), sf), C(5.0)), w[2]), C(1.0)),
                                 self.sub(C(3.0), self.div(sf, w[3]))]
                        axes.append((size, base, w, u))
                    taps = n // 2 + 1
                    total = {3: 16.0, 5: 144.0, 7: 2704.0}[n]
                    acc = None
                    for j in range(taps):
                        for i in range(taps):
                            cx = self.div(self.add(axes[0][1], axes[0][3][i]), axes[0][0])
                            cy = self.div(self.add(axes[1][1], axes[1][3][j]), axes[1][0])
                            cc = self.construct([cx, cy])
                            r = Val(m.op(OpImageSampleDrefExplicitLod, m.t_f32(), si, cc.id, dref.id, 2,
                                         (lod if lod is not None else C(0.0)).id), m.t_f32(), 1)
                            term = self.fmul(self.fmul(axes[0][2][i], axes[1][2][j]), r)
                            acc = term if acc is None else self.add(acc, term)
                    return self.splat(self.fmul(acc, C(1.0 / total)), 4)
                return f

            def is_f(k):
                return Val(m.op(OpIEqual, m.t_bool(), filt.id, m.c_u32(k).id), m.t_bool(), 1)
            cases = [(3, filtered(3)), (5, filtered(5)), (7, filtered(7)), (0, single)]
            if self.narrow:                         # (only the filters seen: NARROW)
                cases = [c for c in cases if c[0] in self.narrow[1]]
            return self.branch([(is_f(f) if i < len(cases) - 1 else None, fn) for i, (f, fn) in enumerate(cases)])

        def is_kind(k):
            return Val(m.op(OpIEqual, m.t_bool(), kind.id, m.c_u32(k).id), m.t_bool(), 1)

        if kind_fixed == 'cube':
            return color(1, 3)()
        if kind_fixed == 'volume':
            return color(2, 3)()
        if kind_fixed == '2d':
            cases = [(3, shadow), (0, color(0, 2))]
        else:
            cases = [(1, color(1, 3)), (2, color(2, 3)), (3, shadow), (0, color(0, 2))]
        if self.narrow:                             # (only the kinds seen at this slot: NARROW)
            ks = self.narrow[0].get(s, {0})
            cases = [c for c in cases if c[0] in ks]
        return self.branch([(is_kind(k) if i < len(cases) - 1 else None, fn) for i, (k, fn) in enumerate(cases)])

    def project(self, s, coord):
        """ps_1_x tex: coord / coord.w when the slot's D3DTTFF_PROJECTED is set (spec constant)"""
        self.tex_bindings()
        m = self.m
        self.proj_used |= 1 << s
        bit = self.spec_field(self.spec_proj, s, 1)
        on = Val(m.op(OpINotEqual, m.t_bool(), bit.id, m.c_u32(0).id), m.t_bool(), 1)
        # (per component, as DXVK writes it: the game projects the race's water colour map by an oT3.w
        # its vertex shader never writes, 0: a division by zero, whose infinities the RTX 4070 turns
        # into texels by the code's shape; VULKAN.md 3.2.3)
        w = self.extract(coord, 3)
        xs = [self.extract(coord, c) for c in range(4)]
        ps = [self.div(x, w) for x in xs]
        return self.construct([self.select(on, p, x) for p, x in zip(ps, xs)])

    def extra(self, i):
        """PS_EXTRA vec4 i (fog, alpha test, bump matrices)"""
        m = self.m
        ptr = m.op(OpAccessChain, m.t_ptr(SC_Uniform, self.fv(4)), self.cbuf, m.c_i32(0).id, m.c_i32(i).id)
        return Val(m.op(OpLoad, self.fv(4), ptr), self.fv(4), 4)

    # ---------------------------------------------------------- the program
    def build(self):
        m = self.m
        void = m.t_void()
        fn = m.new()
        self.fn = fn
        self.entry_label = m.new()
        self.cur_label = self.entry_label
        # the constant buffer
        n = PS_EXTRA + PS_CONSTS if self.ps else VS_CONSTS
        arr = m.t_array(self.fv(4), n, 16)
        st = m.new()
        m.ins(m.glob, OpTypeStruct, st, arr)
        m.deco(st, DEC_Block)
        m.ins(m.annot, OpMemberDecorate, st, 0, DEC_Offset, 0)
        if self.ps:
            self.cbuf = m.gvar(m.t_ptr(SC_Uniform, st), SC_Uniform, 'psc')
        else:                                   # (VS_CHUNKS blocks: an array of them, no stride)
            blocks = m.new()
            m.ins(m.glob, OpTypeArray, blocks, st, m.c_u32(VS_CHUNKS).id)
            self.cbuf = m.gvar(m.t_ptr(SC_Uniform, blocks), SC_Uniform, 'vsc')
        m.deco(self.cbuf, DEC_DescriptorSet, 1)
        m.deco(self.cbuf, DEC_Binding, 1 if self.ps else 0)
        m.deco(self.cbuf, DEC_NonWritable)
        # declarations and definitions first
        for i in self.prog:
            if i.op == d3dasm.OP_DCL:
                self.dcl(i)
            elif i.op == d3dasm.OP_DEF:
                self.defs[(i.dst.typ, i.dst.num)] = m.c_vec([('b', x) for x in i.raw[1:5]])
                if i.dst.typ == R_CONST:
                    self.defs_list.append((i.dst.num, i.raw[1:5]))
            elif i.op == d3dasm.OP_DEFI:
                self.defs[(R_CONSTINT, i.dst.num)] = [struct.unpack('<i', struct.pack('<I', x))[0] for x in i.raw[1:5]]
            elif i.op == d3dasm.OP_DEFB:
                self.defs[(R_CONSTBOOL, i.dst.num)] = i.raw[1]
        if self.ps and self.major == 1:
            # ps_1_x texture registers start as their texture coordinates [DXVK]; r0 is the output
            pass
        prog = [i for i in self.prog if i.op not in (d3dasm.OP_DCL, d3dasm.OP_DEF, d3dasm.OP_DEFI, d3dasm.OP_DEFB)]
        for n, i in enumerate(prog):
            i.idx = n
        flow = any(d3dasm.OPS[i.op][0] in ('rep', 'endrep', 'if', 'ifc', 'else', 'endif', 'loop', 'endloop',
                                            'call', 'callnz', 'ret', 'label', 'break', 'breakc') for i in prog)
        self.fuse = {} if flow else fusions(prog)
        self.fuse_mul = set(self.fuse.values())
        self.captured = {}
        k = 0
        self.flow = []
        while k < len(prog):
            i = prog[k]
            if k + 1 < len(prog) and prog[k + 1].co:
                self.coissue(i, prog[k + 1])
                k += 2
                continue
            self.instr(i)
            k += 1
        if self.flow:
            raise CompileError('unclosed flow control')
        self.finish()
        body = m.code
        m.code = []
        m.ins(m.code, OpFunction, void, fn, 0, m.t_func(void))
        m.ins(m.code, OpLabel, self.entry_label)
        m.code += m.vars
        # registers start at zero; ps_1_x texture registers as their coordinates
        z = m.c_vec([0.0] * 4)
        init = []
        for (typ, num), var in self.regs.items():
            m.ins(init, OpStore, var, z.id)
        # the vertex shader's outputs start as DXVK's defaults, so components a shader never writes
        # read them (they were undefined: the race's water projects by an oT3.w it never writes, 0
        # under DXVK; each driver made up its own): colour 0 (1,1,1,1), other colours (0,0,0,1), fog 1,
        # point size 1, the rest (0,0,0,0)
        for (typ, num), (var, tgt) in self.out_regs.items():
            d = [1.0] * 4 if tgt == VARY_COLOR else [0.0, 0.0, 0.0, 1.0] if tgt == VARY_COLOR + 1 else \
                [1.0, 0.0, 0.0, 0.0] if tgt in (VARY_FOG, 'psize') else [0.0] * 4
            m.ins(init, OpStore, var, m.c_vec(d).id)
        m.code += init + self.pre_init() + body
        m.ins(m.code, OpReturn)
        m.ins(m.code, OpFunctionEnd)
        self.fn_words = m.code

    def pre_init(self):
        return []

    def dcl(self, i):
        u = i.dcl
        d = i.dst
        if d.typ == R_SAMPLER:
            tt = (u >> 27) & 15
            self.samplers[d.num] = {2: '2d', 3: 'cube', 4: 'volume'}.get(tt)
            if self.samplers[d.num] is None:
                raise CompileError('sampler type %d' % tt)
            return
        if self.ps and self.major < 3:
            if d.centroid:                          # ps_2_0's input dcl: by register type (_centroid noted)
                self.centroid_in.add((d.typ, d.num))
            return
        if d.typ == R_MISC:
            return
        if self.ps and d.centroid:
            self.centroid_in.add((d.typ, d.num))
        self.dcl_usage[(d.typ, d.num)] = (u & 31, (u >> 16) & 15)

    def coissue(self, a, b):
        """a ps_1_x co-issued pair: both read their sources before either writes"""
        self.deferred = []
        self.instr(a, defer=True)
        self.instr(b, defer=True)
        for d, v in self.deferred:
            self.dst_store(d, v)
        self.deferred = None

    def result(self, i, v, defer):
        if defer:
            self.deferred.append((i.dst, v))
        else:
            self.dst_store(i.dst, v)

    def instr(self, i, defer=False):
        op, m = i.op, self.m
        name = d3dasm.OPS[op][0]
        S = lambda k, n=4: self.src(i.src[k], n)
        if name == 'nop':
            return
        if name == 'mov':
            v = S(0)
            if i.dst.typ == R_ADDR_TEX and not self.ps:
                self.mova(i, v)
                return
            return self.result(i, v, defer)
        if name == 'mova':
            return self.mova(i, S(0))
        if name in ('add', 'sub') and any((i.idx, si, k) in self.fuse for si in (0, 1) for k in range(4)):
            return self.result(i, self.add_fused(i, name, S(0), S(1)), defer)
        if name == 'mul' and any((i.idx, k) in self.fuse_mul for k in range(4)):
            a2, b2 = self.legacy(S(0), S(1))
            for k in range(4):
                if (i.idx, k) in self.fuse_mul:
                    self.captured[(i.idx, k)] = (self.extract(a2, k), self.extract(b2, k))
            return self.result(i, self.fmul(a2, b2), defer)
        if name in ('add', 'sub', 'mul', 'mad', 'min', 'max', 'slt', 'sge', 'lrp', 'cmp', 'cnd'):
            if name == 'add': v = self.add(S(0), S(1))
            elif name == 'sub': v = self.sub(S(0), S(1))
            elif name == 'mul': v = self.mul(S(0), S(1))
            elif name == 'mad': v = self.mad(S(0), S(1), S(2))
            elif name == 'min': v = self.ext(GLSL_FMin, S(0), S(1))
            elif name == 'max': v = self.ext(GLSL_FMax, S(0), S(1))
            elif name == 'slt': v = self.select(self.cmp(OpFOrdLessThan, S(0), S(1)), self.const(1.0), self.const(0.0))
            elif name == 'sge': v = self.select(self.cmp(OpFOrdGreaterThanEqual, S(0), S(1)), self.const(1.0), self.const(0.0))
            elif name == 'lrp':
                a, b, c = S(0), S(1), S(2)
                v = self.add(self.mul(a, self.sub(b, c)), c)
            elif name == 'cmp':
                v = self.select(self.cmp(OpFOrdGreaterThanEqual, S(0), self.const(*[0.0] * 4)), S(1), S(2))
            else:  # cnd: ps_1_1-1_3 compare r0.a with 0.5 (the source must be r0.a)
                c = S(0)
                v = self.select(self.cmp(OpFOrdGreaterThan, c, self.const(*[0.5] * 4)), S(1), S(2))
            return self.result(i, v, defer)
        if name in ('rcp', 'rsq', 'exp', 'log', 'frc', 'abs', 'expp', 'logp'):
            x = S(0)
            if name == 'rcp': v = self.div(self.const(1.0), x)
            elif name == 'rsq': v = self.ext(GLSL_InverseSqrt, self.ext(GLSL_FAbs, x))
            elif name in ('exp', 'expp'): v = self.ext(GLSL_Exp2, x)
            elif name in ('log', 'logp'): v = self.ext(GLSL_Log2, self.ext(GLSL_FAbs, x))
            elif name == 'frc': v = self.ext(GLSL_Fract, x)
            else: v = self.ext(GLSL_FAbs, x)
            return self.result(i, v, defer)
        if name in ('dp3', 'dp4', 'dp2add'):
            nn = {'dp3': 3, 'dp4': 4, 'dp2add': 2}[name]
            v = self.dot(S(0), S(1), nn, self.extract(S(2), 0) if name == 'dp2add' else None)
            return self.result(i, v, defer)
        if name in ('m4x4', 'm4x3', 'm3x4', 'm3x3', 'm3x2'):
            cols, rows = {'m4x4': (4, 4), 'm4x3': (4, 3), 'm3x4': (3, 4), 'm3x3': (3, 3), 'm3x2': (3, 2)}[name]
            a = S(0)
            comps = []
            for r in range(rows):
                s1 = i.src[1]
                row = Src()
                row.typ, row.num, row.swz, row.mod, row.rel = s1.typ, s1.num + r, s1.swz, s1.mod, s1.rel
                comps.append(self.dot(a, self.src(row), cols))
            while len(comps) < 4:
                comps.append(self.const(0.0))
            d = i.dst
            mask = d.mask & ((1 << rows) - 1)
            dd = Dst()
            dd.typ, dd.num, dd.mask, dd.sat, dd.shift, dd.pp, dd.centroid = d.typ, d.num, mask, d.sat, d.shift, d.pp, False
            v = self.construct(comps)
            if defer:
                self.deferred.append((dd, v))
            else:
                self.dst_store(dd, v)
            return
        if name == 'lit':
            s = S(0)
            x, y, w = self.extract(s, 0), self.extract(s, 1), self.extract(s, 3)
            pw = self.clamp(w, -127.9961, 127.9961)
            zero, one = self.const(0.0), self.const(1.0)
            ok = Val(m.op(OpLogicalAnd, m.t_bool(), self.cmp(OpFOrdGreaterThanEqual, x, zero).id,
                          self.cmp(OpFOrdGreaterThanEqual, y, zero).id), m.t_bool(), 1)
            pz = self.select(ok, self.pow_legacy(y, pw), zero)
            v = self.construct([one, self.ext(GLSL_FMax, x, zero), pz, one])
            return self.result(i, v, defer)
        if name == 'pow':
            a = self.extract(S(0), 0)
            b = self.extract(S(1), 0)
            return self.result(i, self.pow_legacy(self.ext(GLSL_FAbs, a), b), defer)
        if name == 'nrm':
            xyz = S(0, 3)
            sc = self.ext(GLSL_InverseSqrt, self.dot(xyz, xyz, 3))
            return self.result(i, self.mul(S(0), self.splat(sc, 4)), defer)
        if name == 'crs':
            a, b = S(0, 3), S(1, 3)
            ax, ay, az = [self.extract(a, c) for c in range(3)]
            bx, by, bz = [self.extract(b, c) for c in range(3)]
            v = self.construct([self.sub(self.mul(ay, bz), self.mul(az, by)), self.sub(self.mul(az, bx), self.mul(ax, bz)),
                                self.sub(self.mul(ax, by), self.mul(ay, bx)), self.const(0.0)])
            return self.result(i, v, defer)
        if name == 'sincos':
            x = self.extract(S(0), 0)
            v = self.construct([self.ext(GLSL_Cos, x), self.ext(GLSL_Sin, x), self.const(0.0), self.const(0.0)])
            return self.result(i, v, defer)
        if name == 'sgn':
            return self.result(i, self.ext(GLSL_FSign, S(0)), defer)
        if name in ('tex', 'texld', 'texldl'):
            return self.tex(i, defer)
        if name == 'texcoord':
            if self.major >= 2 or self.minor >= 4:
                raise CompileError('texcrd')
            t = self.saturate(self.texcoord_in(i.dst.num))
            t = self.construct([self.extract(t, 0), self.extract(t, 1), self.extract(t, 2), self.const(1.0)])
            return self.result(i, t, defer)
        if name == 'texkill':
            return self.texkill(i)
        if name in ('texbem', 'texbeml'):
            return self.texbem(i, defer)
        if name in ('texm3x2pad',):
            return                                  # (its row is read by texm3x2tex)
        if name == 'texm3x2tex':
            n = i.dst.num
            tn = self.src(i.src[0], 3)
            u = self.dot(self.texcoord_in(n - 1), tn, 3)
            w = self.dot(self.texcoord_in(n), tn, 3)
            coord = self.construct([u, w, self.const(0.0), self.const(0.0)])
            self.samplers.setdefault(n, None)
            return self.result(i, self.sample(n, coord, self.samplers.get(n)), defer)
        if name == 'rep':
            return self.rep(i)
        if name == 'endrep':
            return self.endrep()
        if name in ('ifc', 'if'):
            return self.if_(i)
        if name == 'else':
            return self.else_()
        if name == 'endif':
            return self.endif()
        if name == 'dsx' or name == 'dsy':
            raise CompileError(name)
        raise CompileError('instruction %s' % name)

    def add_fused(self, i, name, x, y):
        """an add or sub with a multiply's single-use result fused in per component (fusions): Fma of the
        multiply's operands (legacy-selected, captured at the multiply) onto the other operand"""
        comps = []
        for k in range(4):
            xk, yk = self.extract(x, k), self.extract(y, k)
            f0, f1 = (i.idx, 0, k) in self.fuse, (i.idx, 1, k) in self.fuse
            if not (i.dst.mask >> k & 1) or not (f0 or f1):
                comps.append(self.add(xk, yk) if name == 'add' else self.sub(xk, yk))
                continue
            si = 0 if f0 else 1
            a, b = self.captured[self.fuse[(i.idx, si, k)]]
            other = yk if si == 0 else xk
            if name == 'sub' and si == 0:               # a x b - c
                other = self.neg(other)
            elif name == 'sub':                          # c - a x b
                a = self.neg(a)
            comps.append(self.ext(GLSL_Fma, a, b, other))
        return self.construct(comps)

    def pow_legacy(self, a, b):
        """|a|^b as Direct3D: exp2(b * log2(a)) with the legacy multiply (0^0 = 1)"""
        return self.ext(GLSL_Exp2, self.mul(b, self.ext(GLSL_Log2, a)))

    def mova(self, i, v):
        m = self.m
        r = self.ext(GLSL_RoundEven, v) if self.major >= 2 else self.ext(GLSL_Floor, v)
        it = m.t_vec(m.t_i32(), 4)
        iv = m.op(OpConvertFToS, it, r.id)
        var = self.addr_var()
        old = m.op(OpLoad, it, var)
        comps = [4 + c if i.dst.mask >> c & 1 else c for c in range(4)]
        new = m.op(OpVectorShuffle, it, old, iv, *comps)
        m.ins(m.code, OpStore, var, new)

    def tex(self, i, defer):
        m = self.m
        name = d3dasm.OPS[i.op][0]
        if self.ps and self.major == 1:
            if self.minor >= 4:
                raise CompileError('ps_1_4')
            s = i.dst.num
            coord = self.project(s, self.texcoord_in(s))
            self.samplers.setdefault(s, None)
            return self.result(i, self.sample(s, coord, None), defer)
        if not self.ps:
            raise CompileError('vertex texture')
        coord = self.src(i.src[0])
        s = i.src[1].num
        kind = self.samplers.get(s)
        if s not in self.samplers:
            raise CompileError('sampler s%d not declared' % s)
        lod = bias = None
        if name == 'texldl':
            lod = self.extract(coord, 3)
        elif i.ctrl == 1:                           # texldp
            coord = self.div(coord, self.splat(self.extract(coord, 3), 4))
        elif i.ctrl == 2:                           # texldb
            bias = self.extract(coord, 3)
        v = self.sample(s, coord, kind, lod=lod, bias=bias)
        if self.major >= 3:
            v = self.shuffle(v, i.src[1].swz)
        return self.result(i, v, defer)

    def texkill(self, i):
        m = self.m
        self.kills = True
        if self.major == 1:
            v = self.texcoord_in(i.dst.num)
            mask = i.dst.mask & 7
        else:
            v = self.load_reg(i.dst.typ, i.dst.num) if i.dst.typ == R_TEMP else self.ps_input(i.dst.typ, i.dst.num)
            mask = i.dst.mask
        mn = None
        for c in range(4):
            if mask >> c & 1:
                x = self.extract(v, c)
                mn = x if mn is None else self.ext(GLSL_FMin, x, mn)
        cond = self.cmp(OpFOrdLessThan, mn, self.const(0.0))
        lt, merge = m.new(), m.new()
        m.ins(m.code, OpSelectionMerge, merge, 0)
        m.ins(m.code, OpBranchConditional, cond.id, lt, merge)
        self.lab(lt)
        m.ins(m.code, OpDemoteToHelperInvocation)
        m.ins(m.code, OpBranch, merge)
        self.lab(merge)

    def texbem(self, i, defer):
        """texbem tm, tn: tm's coordinate moved by the bump matrix (D3DTSS_BUMPENVMAT of stage m) times tn"""
        m = self.m
        s = i.dst.num
        coord = self.project(s, self.texcoord_in(s))
        t = self.src(i.src[0])
        mat = self.extra(2 + s) if s < 4 else None
        if mat is None:
            raise CompileError('texbem stage %d' % s)
        tx, ty = self.extract(t, 0), self.extract(t, 1)
        # (DXVK's order: the coordinate plus M00 du, plus M10 dv)
        u = self.add(self.add(self.extract(coord, 0), self.mul(self.extract(mat, 0), tx)), self.mul(self.extract(mat, 2), ty))
        v = self.add(self.add(self.extract(coord, 1), self.mul(self.extract(mat, 1), tx)), self.mul(self.extract(mat, 3), ty))
        c = self.construct([u, v, self.extract(coord, 2), self.extract(coord, 3)])
        self.samplers.setdefault(s, None)
        r = self.sample(s, c, None)
        if d3dasm.OPS[i.op][0] == 'texbeml':
            raise CompileError('texbeml')
        return self.result(i, r, defer)

    # structured flow (ps_3_0: rep with an immediate count, ifc/if, else)
    def rep(self, i):
        m = self.m
        cnt = self.src_raw(i.src[0])
        if not isinstance(cnt, list):
            raise CompileError('rep count not an immediate defi')
        n = cnt[i.src[0].swz[0]]
        ctr = m.fvar(m.t_i32(), 'rep%d' % len(self.flow))
        m.ins(m.code, OpStore, ctr, m.c_i32(n).id)
        header, body, cont, merge = m.new(), m.new(), m.new(), m.new()
        m.ins(m.code, OpBranch, header)
        self.lab(header)
        c = m.op(OpLoad, m.t_i32(), ctr)
        go = m.op(OpSLessThan, m.t_bool(), m.c_i32(0).id, c)
        m.ins(m.code, OpLoopMerge, merge, cont, 0)
        m.ins(m.code, OpBranchConditional, go, body, merge)
        self.lab(body)
        self.flow.append(('rep', ctr, header, cont, merge))

    def endrep(self):
        m = self.m
        kind, ctr, header, cont, merge = self.flow.pop()
        if kind != 'rep':
            raise CompileError('endrep without rep')
        m.ins(m.code, OpBranch, cont)
        self.lab(cont)
        c = m.op(OpLoad, m.t_i32(), ctr)
        m.ins(m.code, OpStore, ctr, m.op(OpISub, m.t_i32(), c, m.c_i32(1).id))
        m.ins(m.code, OpBranch, header)
        self.lab(merge)

    def if_(self, i):
        m = self.m
        if d3dasm.OPS[i.op][0] == 'if':
            raise CompileError('if on a bool register')
        a, b = self.extract(self.src(i.src[0]), 0), self.extract(self.src(i.src[1]), 0)
        op = {1: OpFOrdGreaterThan, 2: OpFOrdEqual, 3: OpFOrdGreaterThanEqual, 4: OpFOrdLessThan,
              5: OpFUnordNotEqual, 6: OpFOrdLessThanEqual}[i.ctrl]
        c = self.cmp(op, a, b)
        lt, lf, merge = m.new(), m.new(), m.new()
        m.ins(m.code, OpSelectionMerge, merge, 0)
        m.ins(m.code, OpBranchConditional, c.id, lt, lf)
        self.lab(lt)
        self.flow.append(['if', lf, merge, False])

    def else_(self):
        m = self.m
        f = self.flow[-1]
        if f[0] != 'if' or f[3]:
            raise CompileError('else without if')
        m.ins(m.code, OpBranch, f[2])
        self.lab(f[1])
        f[3] = True

    def endif(self):
        m = self.m
        f = self.flow.pop()
        if f[0] != 'if':
            raise CompileError('endif without if')
        m.ins(m.code, OpBranch, f[2])
        if not f[3]:
            self.lab(f[1])
            m.ins(m.code, OpBranch, f[2])
        self.lab(f[2])

    # ---------------------------------------------------------- the end of the program
    def finish(self):
        if self.ps:
            self.finish_ps()
        else:
            self.finish_vs()

    def finish_vs(self):
        m = self.m
        # position, point size
        pos = m.gvar(m.t_ptr(SC_Output, self.fv(4)), SC_Output, 'Position')
        m.deco(pos, DEC_BuiltIn, 0)
        psz = m.gvar(m.t_ptr(SC_Output, m.t_f32()), SC_Output, 'PointSize')
        m.deco(psz, DEC_BuiltIn, 1)
        outs = {}
        for (typ, num), (var, tgt) in self.out_regs.items():
            outs[tgt] = var
        if 'pos' in outs:
            m.ins(m.code, OpStore, pos, m.op(OpLoad, self.fv(4), outs['pos']))
        else:
            m.ins(m.code, OpStore, pos, m.c_vec([0.0, 0.0, 0.0, 1.0]).id)
        if 'psize' in outs:
            v = self.extract(Val(m.op(OpLoad, self.fv(4), outs['psize']), self.fv(4), 4), 0)
            m.ins(m.code, OpStore, psz, v.id)
        else:
            m.ins(m.code, OpStore, psz, self.const(1.0).id)
        for loc in range(NVARY):
            if loc == VARY_FOG:
                var = m.gvar(m.t_ptr(SC_Output, m.t_f32()), SC_Output, 'oFog')
                m.deco(var, DEC_Location, loc)
                if loc in outs:
                    v = self.extract(Val(m.op(OpLoad, self.fv(4), outs[loc]), self.fv(4), 4), 0)
                else:
                    v = self.const(1.0)
                m.ins(m.code, OpStore, var, v.id)
                continue
            var = m.gvar(m.t_ptr(SC_Output, self.fv(4)), SC_Output, 'vary%d' % loc)
            m.deco(var, DEC_Location, loc)
            if loc in outs:
                v = Val(m.op(OpLoad, self.fv(4), outs[loc]), self.fv(4), 4)
                if loc < VARY_TEX and self.major < 3:
                    v = self.saturate(v)
            elif loc == VARY_COLOR:
                v = m.c_vec([1.0, 1.0, 1.0, 1.0])
            elif loc == VARY_COLOR + 1:
                v = m.c_vec([0.0, 0.0, 0.0, 1.0])
            else:
                v = m.c_vec([0.0, 0.0, 0.0, 0.0])
            m.ins(m.code, OpStore, var, v.id)

    def finish_ps(self):
        m = self.m
        out = m.gvar(m.t_ptr(SC_Output, self.fv(4)), SC_Output, 'oC0')
        m.deco(out, DEC_Location, 0)
        if self.major == 1:
            col = self.load_reg(R_TEMP, 0)
        elif (R_COLOROUT, 0) in self.regs:
            col = self.load_reg(R_COLOROUT, 0)
        else:
            col = m.c_vec([0.0, 0.0, 0.0, 0.0])
        # fog (not in shader model 3): spec 1: 0 off, 1 the vertex shader's fog, 2 exp, 3 exp2, 4 linear
        if self.major < 3:
            col = self.fog(col)
        self.alpha_test(col)
        m.ins(m.code, OpStore, out, col.id)
        if self.writes_depth:
            dv = m.gvar(m.t_ptr(SC_Output, m.t_f32()), SC_Output, 'FragDepth')
            m.deco(dv, DEC_BuiltIn, 22)
            m.ins(m.code, OpStore, dv, self.extract(self.load_reg(R_DEPTHOUT, 0), 0).id)

    def fog(self, col):
        m = self.m
        spec = m.spec_u32(SPEC_FOG, 0)
        on = Val(m.op(OpINotEqual, m.t_bool(), spec.id, m.c_u32(0).id), m.t_bool(), 1)

        def fogged():
            e0, e1 = self.extra(0), self.extra(1)
            fc = self.frag_coord()
            depth = self.div(self.const(1.0), self.extract(fc, 3))     # W fog (a vertex shader: never Z)
            # the vertex shader's fog coordinate
            if not hasattr(self, '_vfog'):
                self._vfog = m.gvar(m.t_ptr(SC_Input, m.t_f32()), SC_Input, 'vFog')
                m.deco(self._vfog, DEC_Location, VARY_FOG)
            vf = Val(m.op(OpLoad, m.t_f32(), self._vfog), m.t_f32(), 1)
            lin = self.mul(self.sub(self.extract(e1, 0), depth), self.extract(e1, 1))
            d1 = self.mul(depth, self.extract(e0, 3))
            ex = self.ext(GLSL_Exp2, self.fmul(d1, self.const(-1.442695041)))
            ex2 = self.ext(GLSL_Exp2, self.fmul(self.mul(d1, d1), self.const(-1.442695041)))
            is_ = lambda k: Val(m.op(OpIEqual, m.t_bool(), spec.id, m.c_u32(k).id), m.t_bool(), 1)
            f = self.select(is_(4), lin, self.select(is_(3), ex2, self.select(is_(2), ex, vf)))
            f = self.ext(GLSL_FMax, self.ext(GLSL_FMin, f, self.const(1.0)), self.const(0.0))
            fcol = self.shuffle(e0, [0, 1, 2])
            rgb = self.add(fcol, self.fmul(self.sub(self.shuffle(col, [0, 1, 2]), fcol), self.splat(f, 3)))
            return self.construct([self.extract(rgb, 0), self.extract(rgb, 1), self.extract(rgb, 2), self.extract(col, 3)])
        return self.branch([(on, fogged), (None, lambda: col)])

    def alpha_test(self, col):
        """Direct3D's alpha test (spec 0: the D3DCMP function, 8 = always); in 8 bits as DXVK for
        8-bit targets (extra[1].w 255), else as floats"""
        m = self.m
        spec = m.spec_u32(SPEC_ALPHA, 8)
        on = Val(m.op(OpULessThan, m.t_bool(), spec.id, m.c_u32(8).id), m.t_bool(), 1)
        lt, merge = m.new(), m.new()
        m.ins(m.code, OpSelectionMerge, merge, 0)
        m.ins(m.code, OpBranchConditional, on.id, lt, merge)
        self.lab(lt)
        e1 = self.extra(1)
        a = self.extract(col, 3)
        scale = self.extract(e1, 3)
        ref = self.extract(e1, 2)
        ints = self.cmp(OpFOrdGreaterThan, scale, self.const(0.0))
        a = self.select(ints, self.ext(GLSL_RoundEven, self.fmul(a, scale)), a)
        is_ = lambda k: Val(m.op(OpIEqual, m.t_bool(), spec.id, m.c_u32(k).id), m.t_bool(), 1)
        tests = {2: OpFOrdLessThan, 3: OpFOrdEqual, 4: OpFOrdLessThanEqual, 5: OpFOrdGreaterThan,
                 6: OpFOrdNotEqual, 7: OpFOrdGreaterThanEqual}
        passed = self.m.c_bool(False)          # 1 never
        for k, op in tests.items():
            c = self.cmp(op, a, ref)
            passed = Val(m.op(OpSelect, m.t_bool(), is_(k).id, c.id, passed.id), m.t_bool(), 1)
        kill, merge2 = m.new(), m.new()
        m.ins(m.code, OpSelectionMerge, merge2, 0)
        m.ins(m.code, OpBranchConditional, passed.id, merge2, kill)
        self.lab(kill)
        m.ins(m.code, OpDemoteToHelperInvocation)
        m.ins(m.code, OpBranch, merge2)
        self.lab(merge2)
        m.ins(m.code, OpBranch, merge)
        self.lab(merge)

    # ---------------------------------------------------------- output
    def spirv(self):
        m = self.m
        model = 4 if self.ps else 0
        modes = [[7]] if self.ps else []                 # OriginUpperLeft
        if self.writes_depth:
            modes.append([12])                           # DepthReplacing
        # float controls as DXVK's sm3 shaders ask for them: round to nearest even, denormals flushed and
        # (SPV_KHR_float_controls2) a default fast-math mode allowing everything (contract, reassociate,
        # reciprocals, transforms) and NSZ; runtime/vk_d3d9.c module_of keeps them only where DXVK uses
        # them (variant below)
        m.caps += [4467, 4465, 6029]                     # RoundingModeRTE, DenormFlushToZero, FloatControls2
        m.exts += ['SPV_KHR_float_controls2']
        modes += [[4462, 32], [4460, 32], [331, 6028, m.t_f32(), m.c_u32(FC2_DEFAULT).id]]
        words = m.words(model, self.fn, modes) + self.fn_words
        words[3] = m.bound
        return words

    def meta(self):
        d = dict(ps=self.ps, major=self.major, minor=self.minor, cmax=self.cmax, rel=self.rel,
                 inputs=sorted(self.inputs.items()), samplers=dict(self.samplers), proj=self.proj_used,
                 defs=self.defs_list, depth=self.writes_depth, kill=self.kills)
        return d


# ------------------------------------------------------------------ tools
def compile_bytes(code, name='', narrow=None):
    s = Shader(code, name, narrow)
    return s.spirv(), s.meta()


# NARROW VARIANTS (round 3, VULKAN.md 2.2). A pixel shader's every sampling site carries the paths for
# each kind of texture its slot may hold (ps_1_x declares none: 2D, cube, volume, depth compare) and
# each compare path the three shadow filters, all chosen by specialization constants: most of the
# 1.26 M words, which every driver converts before it drops the paths not taken. data/vk_kinds.txt
# lists, per pixel shader, the kinds each slot was seen with and the filters seen (tools/vk_kinds.py,
# from the pipelines our renderer made: vk_pipelines.bin); such a shader also gets a variant with only
# those paths, used for a pipeline whose kinds and filter are among them (vk_d3d9.c module_of), the
# full one otherwise. The kept paths are the same code, so the pictures are the same.
def read_kinds(path):
    """{hash: ({slot: set(kinds)}, set(filters))} from data/vk_kinds.txt"""
    out = {}
    if not path or not os.path.exists(path):
        return out
    for line in open(path):
        line = line.split('#')[0].split()
        if not line:
            continue
        h = int(line[0], 16)
        ks, fs = {}, {0}
        for f in line[1:]:
            k, v = f.split('=')
            vals = {int(x) for x in v.split(',') if x}
            if k == 'f':
                fs = vals
            else:
                ks[int(k)] = vals
        out[h] = (ks, fs)
    return out


def narrow_masks(nk):
    """the runtime's masks: 4 bits a slot (bit k: kind k), the filters (bit 0: none, 1: 3, 2: 5, 3: 7)"""
    ks, fs = nk
    km = 0
    for sl in range(8):
        km |= sum(1 << k for k in ks.get(sl, {0})) << (4 * sl)
    fm = sum({0: 1, 3: 2, 5: 4, 7: 8}[f] for f in fs)
    return km, fm


def variant(words, fcon, fc2):
    """the module as the runtime hands it to a device (runtime/vk_d3d9.c module_of, the same rules), as
    DXVK decides: fcon (independent denormal modes, 32-bit RTE and flush-to-zero) keeps RTE and
    flush-to-zero, fc2 (fcon, signed zero/inf/NaN preserve and float_controls2) keeps the default
    fast-math mode, with the precise operations' NoContraction as FPFastMathMode 0 (NoContraction is
    not allowed beside it); otherwise those go and NoContraction stays"""
    out = words[:5]
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        ins = words[i:i + n]
        drop = False
        if op == 71 and n == 3 and ins[2] == 42 and fc2:     # OpDecorate NoContraction -> FPFastMathMode 0
            ins = [(4 << 16) | 71, ins[1], 40, 0]
        elif op == 17:                                       # OpCapability
            drop = (ins[1] == 6029 and not fc2) or (ins[1] in (4467, 4465) and not fcon)
        elif op == 10:                                       # OpExtension
            drop = not fc2 and spv_string('SPV_KHR_float_controls2') == ins[1:]
        elif op == 331:                                      # OpExecutionModeId
            drop = ins[2] == 6028 and not fc2
        elif op == 16:                                       # OpExecutionMode
            drop = ins[2] in (4462, 4460) and not fcon
        if not drop:
            out += ins
        i += n
    return out


def validate(words, label):
    """spirv-val on the module's variants: float controls with float_controls2 (RADV), none (NVIDIA),
    the modes without float_controls2 (variant)"""
    for v in (variant(words, True, True), variant(words, False, False), variant(words, True, False)):
        r = validate1(v, label)
        if r:
            return r
    return r


def validate1(words, label):
    exe = None
    for p in os.environ.get('PATH', '').split(':'):
        if os.path.exists(os.path.join(p, 'spirv-val')):
            exe = os.path.join(p, 'spirv-val')
            break
    if not exe:
        return None
    with tempfile.NamedTemporaryFile(suffix='.spv', delete=False) as f:
        f.write(struct.pack('<%dI' % len(words), *words))
        path = f.name
    r = subprocess.run([exe, '--target-env', 'vulkan1.3', path], capture_output=True, text=True)
    os.unlink(path)
    if r.returncode:
        return (r.stdout + r.stderr).strip()
    return ''


def sources(game, subs, captured):
    """(label, key bytecode) for every shader the device can be given (the docstring at the top)"""
    out = []
    for d in (os.path.join(game, 'Shaders'), subs):
        if not d or not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.lower().endswith('.vsh') or f.lower().endswith('.psh'):
                try:
                    out.append((os.path.join(d, f), d3dasm.assemble(open(os.path.join(d, f), errors='replace').read())))
                except d3dasm.AsmError as e:
                    print('d3d2spv: %s/%s not assembled (%s): left out' % (d, f, e))
    if captured and os.path.isdir(captured):
        for f in sorted(os.listdir(captured)):
            if f.endswith('.bin'):
                out.append((os.path.join(captured, f), open(os.path.join(captured, f), 'rb').read()))
    return out


def cmd_table(game, subs, captured, out, kinds_file=None):
    seen = {}
    words_all, entries, defs_all = [], [], []
    errors = []
    kinds = read_kinds(kinds_file)
    nfull = nnarrow = 0
    for label, b in sources(game, subs, captured):
        h = d3dasm.fnv1a64(b)
        if h in seen:
            continue
        seen[h] = label
        try:
            w, meta = compile_bytes(b, label)
            nw = compile_bytes(b, label, kinds[h])[0] if h in kinds and meta['ps'] else None
        except (CompileError, d3dasm.AsmError) as e:
            errors.append('%s: %s' % (label, e))
            continue
        v = validate(w, label) or (validate(nw, label + ' (narrow)') if nw else '')
        if v:
            errors.append('%s: spirv-val: %s' % (label, v.splitlines()[0] if v else ''))
            continue
        meta['narrow'] = None
        if nw and len(nw) < len(w):
            meta['narrow'] = (len(words_all) + len(w), len(nw)) + narrow_masks(kinds[h])
            nfull += len(w)
            nnarrow += len(nw)
        entries.append((h, label, len(words_all), len(w), meta))
        words_all += w
        if meta['narrow']:
            words_all += nw
    if errors:
        sys.exit('d3d2spv: %d shader(s) failed:\n  %s' % (len(errors), '\n  '.join(errors)))
    lines = ['/* Generated by tools/d3d2spv.py: every shader as SPIR-V, keyed by the FNV-1a 64 hash of the',
             '   Direct3D bytecode (runtime/vk_d3d9.c module_of). Do not edit. */',
             '#include "vk_abi.h"', '']
    lines.append('const uint32_t vk_spirv_words[] = {')
    for k in range(0, len(words_all), 8):
        lines.append('    ' + ', '.join('0x%08x' % x for x in words_all[k:k + 8]) + ',')
    lines.append('};')
    ndef = 0
    lines.append('const VkShaderDef vk_shader_defs[] = {')
    defo = []
    for h, label, off, n, meta in entries:
        defo.append(ndef)
        for reg, bits in meta['defs']:
            lines.append('    {%d, {0x%08x, 0x%08x, 0x%08x, 0x%08x}},' % ((reg,) + tuple(bits)))
            ndef += 1
    lines.append('    {0, {0, 0, 0, 0}}};')
    lines.append('const VkShaderMeta vk_shader_meta[] = {')
    for (h, label, off, n, meta), do in zip(entries, defo):
        inputs = 0
        for loc, (u, idx) in meta['inputs']:
            inputs |= 1 << loc
        smask = 0
        kinds = 0
        for s, k in meta['samplers'].items():
            smask |= 1 << s
            kinds |= {None: 0, '2d': 1, 'cube': 2, 'volume': 3}[k] << (2 * s)
        flags = (1 if meta['ps'] else 0) | (2 if meta['rel'] else 0) | (4 if meta['depth'] else 0) | (8 if meta['kill'] else 0)
        no, nn, nkm, nfm = meta['narrow'] or (0, 0, 0, 0)
        lines.append('    {0x%016xull, %d, %d, %d, %d, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, %d, %d, %d, %d, %d, 0x%x, 0x%x}, /* %s */' % (
            h, off, n, meta['major'], meta['minor'], flags, inputs, smask, kinds, meta['proj'], meta['cmax'],
            do, len(meta['defs']), no, nn, nkm, nfm, label))
    lines.append('};')
    lines.append('const int vk_shader_count = %d;' % len(entries))
    text = '\n'.join(lines) + '\n'
    if not os.path.exists(out) or open(out).read() != text:
        with open(out, 'w', newline='\n') as f:
            f.write(text)
        print('d3d2spv: %d shaders compiled into %s (%d words; %d shaders with a narrow variant: %d words '
              'instead of %d)' % (len(entries), out, len(words_all), sum(1 for e in entries if e[4]['narrow']),
                                  nnarrow, nfull))


def cmd_one(path, out):
    labels = d3dasm.read_any(path) if not (path.endswith('.vsh') or path.endswith('.psh')) else \
        [(path, d3dasm.assemble(open(path).read()))]
    for label, b in labels[:1]:
        w, meta = compile_bytes(b, label)
        v = validate(w, label)
        print('%s: %d words, %s%s' % (label, len(w), meta, ('; spirv-val: ' + v) if v else ''))
        if out:
            with open(out, 'wb') as f:
                f.write(struct.pack('<%dI' % len(w), *w))


def cmd_check(game):
    bad = 0
    n = 0
    for label, b in sources(game, 'data/shaders', os.path.expanduser('~/.local/share/etm-native/shaders')):
        n += 1
        try:
            w, meta = compile_bytes(b, label)
            v = validate(w, label)
            if v:
                bad += 1
                print('%s: spirv-val: %s' % (label, v))
        except (CompileError, d3dasm.AsmError) as e:
            bad += 1
            print('%s: %s' % (label, e))
    print('%d shaders: %s' % (n, 'all compiled and valid' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


def main(a):
    if not a:
        print(__doc__)
        return 2
    if a[0] == 'table' and len(a) in (5, 6):
        return cmd_table(*a[1:])
    if a[0] == 'one':
        out = a[a.index('-o') + 1] if '-o' in a else None
        return cmd_one(a[1], out)
    if a[0] == 'check':
        return cmd_check(a[1] if len(a) > 1 else d3dasm.DEFAULT_GAME)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
