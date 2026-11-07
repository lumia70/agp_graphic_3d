"""Generate the Lume reflection blob (`<shader>.spv.lsb`) for LumeGS SPIR-V.

The upstream GN build compiles GS shaders with LumeShaderCompiler, which emits
both `.spv` and `.lsb`. LumeApp only has the SDK glslang validator, so this
script reflects the generated SPIR-V itself. The format is documented in
`lume/LumeBinaryCompile/LumeShaderCompiler/README.md` (version 1).

Usage:
  python gen_gs_lsb.py [--spv <staging dir>]
"""

import argparse
import os
import struct
import sys

# SPIR-V opcodes.
OP_NAME = 5
OP_MEMBER_NAME = 6
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_TYPE_BOOL = 20
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_MATRIX = 24
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_TYPE_ACCELERATION_STRUCTURE_KHR = 5341
OP_CONSTANT = 43
OP_SPEC_CONSTANT_TRUE = 48
OP_SPEC_CONSTANT_FALSE = 49
OP_SPEC_CONSTANT = 50
OP_SPEC_CONSTANT_OP = 51
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_VARIABLE = 59

# SPIR-V decorations.
DECORATION_SPEC_ID = 1
DECORATION_BUFFER_BLOCK = 3
DECORATION_OFFSET = 35
DECORATION_ARRAY_STRIDE = 6
DECORATION_BUILT_IN = 11
DECORATION_LOCATION = 30
DECORATION_BINDING = 33
DECORATION_DESCRIPTOR_SET = 34

# Storage classes.
STORAGE_UNIFORM_CONSTANT = 0
STORAGE_INPUT = 1
STORAGE_UNIFORM = 2
STORAGE_PUSH_CONSTANT = 9
STORAGE_STORAGE_BUFFER = 12

EXECUTION_MODE_LOCAL_SIZE = 17

# Descriptor types (render/device/pipeline_state_desc.h).
DESCRIPTOR_TYPE_SAMPLER = 0
DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1
DESCRIPTOR_TYPE_SAMPLED_IMAGE = 2
DESCRIPTOR_TYPE_STORAGE_IMAGE = 3
DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6
DESCRIPTOR_TYPE_STORAGE_BUFFER = 7
DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE = 1000150000

# Shader stage flags (render/device/pipeline_state_desc.h).
STAGE_VERTEX = 0x1
STAGE_FRAGMENT = 0x10
STAGE_COMPUTE = 0x20

# Specialization constant types (render/device/pipeline_state_desc.h).
SPEC_BOOL = 1
SPEC_UINT32 = 2
SPEC_INT32 = 3
SPEC_FLOAT = 4

# Formats (base/util/formats.h).
BASE_FORMAT_R32_UINT = 98
BASE_FORMAT_R32_SINT = 99
BASE_FORMAT_R32_SFLOAT = 100
BASE_FORMAT_R32G32_UINT = 101
BASE_FORMAT_R32G32_SINT = 102
BASE_FORMAT_R32G32_SFLOAT = 103
BASE_FORMAT_R32G32B32_UINT = 104
BASE_FORMAT_R32G32B32_SINT = 105
BASE_FORMAT_R32G32B32_SFLOAT = 106
BASE_FORMAT_R32G32B32A32_UINT = 107
BASE_FORMAT_R32G32B32A32_SINT = 108
BASE_FORMAT_R32G32B32A32_SFLOAT = 109
BASE_FORMAT_R16_SFLOAT = 76
BASE_FORMAT_R16G16_SFLOAT = 83
BASE_FORMAT_R16G16B16_SFLOAT = 90
BASE_FORMAT_R16G16B16A16_SFLOAT = 97

FORMAT_TABLE = {
    (1, 32, False): BASE_FORMAT_R32_UINT,
    (1, 32, True): BASE_FORMAT_R32_SINT,
    (2, 32, False): BASE_FORMAT_R32G32_UINT,
    (2, 32, True): BASE_FORMAT_R32G32_SINT,
    (3, 32, False): BASE_FORMAT_R32G32B32_UINT,
    (3, 32, True): BASE_FORMAT_R32G32B32_SINT,
    (4, 32, False): BASE_FORMAT_R32G32B32A32_UINT,
    (4, 32, True): BASE_FORMAT_R32G32B32A32_SINT,
}

FLOAT_FORMAT_TABLE = {
    1: BASE_FORMAT_R32_SFLOAT,
    2: BASE_FORMAT_R32G32_SFLOAT,
    3: BASE_FORMAT_R32G32B32_SFLOAT,
    4: BASE_FORMAT_R32G32B32A32_SFLOAT,
}

HALF_FORMAT_TABLE = {
    1: BASE_FORMAT_R16_SFLOAT,
    2: BASE_FORMAT_R16G16_SFLOAT,
    3: BASE_FORMAT_R16G16B16_SFLOAT,
    4: BASE_FORMAT_R16G16B16A16_SFLOAT,
}


def _u16(value):
    return struct.pack("<H", value & 0xFFFF)


def _u32(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


class Spirv:
    def __init__(self, words):
        self.words = words
        self.types = {}
        self.constants = {}
        self.decorations = {}
        self.member_decorations = {}
        self.variables = []
        self.local_size = None
        self._parse()

    def _parse(self):
        i = 5  # SPIR-V header
        while i < len(self.words):
            word = self.words[i]
            count = word >> 16
            opcode = word & 0xFFFF
            if count == 0:
                break
            operands = self.words[i + 1:i + count]
            self._handle(opcode, operands)
            i += count

    def _handle(self, opcode, ops):
        if opcode in (OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX,
                      OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY,
                      OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER,
                      OP_TYPE_ACCELERATION_STRUCTURE_KHR):
            if ops:
                self.types[ops[0]] = (opcode, ops)
        elif opcode in (OP_CONSTANT, OP_SPEC_CONSTANT, OP_SPEC_CONSTANT_TRUE,
                        OP_SPEC_CONSTANT_FALSE, OP_SPEC_CONSTANT_OP):
            if len(ops) >= 2:
                self.constants[ops[1]] = (opcode, ops[0], ops[2:])
        elif opcode == OP_DECORATE:
            if len(ops) >= 2:
                self.decorations.setdefault(ops[0], {})[ops[1]] = ops[2:]
        elif opcode == OP_MEMBER_DECORATE:
            if len(ops) >= 3:
                self.member_decorations.setdefault((ops[0], ops[1]), {})[ops[2]] = ops[3:]
        elif opcode == OP_VARIABLE:
            if len(ops) >= 3:
                self.variables.append((ops[0], ops[1], ops[2]))
        elif opcode == OP_EXECUTION_MODE:
            if len(ops) >= 2 and ops[1] == EXECUTION_MODE_LOCAL_SIZE and len(ops) >= 5:
                self.local_size = (ops[2], ops[3], ops[4])

    def type_of(self, type_id):
        return self.types.get(type_id)

    def decoration(self, target, decoration):
        return self.decorations.get(target, {}).get(decoration)

    def member_decoration(self, struct_id, member, decoration):
        return self.member_decorations.get((struct_id, member), {}).get(decoration)

    def constant_value(self, id_):
        entry = self.constants.get(id_)
        if not entry:
            return None
        opcode, _type_id, values = entry
        if opcode in (OP_SPEC_CONSTANT_TRUE,):
            return 1
        if opcode in (OP_SPEC_CONSTANT_FALSE,):
            return 0
        if values:
            return values[0]
        return None

    def type_size(self, type_id):
        entry = self.type_of(type_id)
        if not entry:
            return 0
        opcode, ops = entry
        if opcode == OP_TYPE_BOOL:
            return 4
        if opcode in (OP_TYPE_INT, OP_TYPE_FLOAT):
            return ops[1] // 8
        if opcode == OP_TYPE_VECTOR:
            return self.type_size(ops[1]) * ops[2]
        if opcode == OP_TYPE_MATRIX:
            return self.type_size(ops[1]) * ops[2]
        if opcode in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            element = self.type_size(ops[1])
            if opcode == OP_TYPE_ARRAY:
                length = self.constant_value(ops[2]) or 0
                return element * length
            return element
        if opcode == OP_TYPE_STRUCT:
            size = 0
            for index, member_type in enumerate(ops[1:]):
                offset = self.member_decoration(type_id, index, DECORATION_OFFSET)
                member_size = self.type_size(member_type)
                if offset:
                    size = max(size, offset[0] + member_size)
                else:
                    size += member_size
            return size
        return 0

    def struct_buffer_kind(self, struct_id, storage):
        if storage == STORAGE_STORAGE_BUFFER:
            return DESCRIPTOR_TYPE_STORAGE_BUFFER
        if self.decoration(struct_id, DECORATION_BUFFER_BLOCK) is not None:
            return DESCRIPTOR_TYPE_STORAGE_BUFFER
        return DESCRIPTOR_TYPE_UNIFORM_BUFFER

    def image_info(self, image_type_id):
        entry = self.type_of(image_type_id)
        if not entry or entry[0] != OP_TYPE_IMAGE:
            return 1, 0
        ops = entry[1]
        dim = ops[2] if len(ops) > 2 else 1
        depth = ops[3] if len(ops) > 3 else 0
        arrayed = ops[4] if len(ops) > 4 else 0
        ms = ops[5] if len(ops) > 5 else 0
        sampled = ops[6] if len(ops) > 6 else 0
        flags = 0
        if depth:
            flags |= 0x1
        if arrayed:
            flags |= 0x2
        if ms:
            flags |= 0x4
        if sampled == 1:
            flags |= 0x8
        elif sampled == 2:
            flags |= 0x10
        return dim, flags

    def unwrap_array(self, type_id):
        count = 1
        current = type_id
        while True:
            entry = self.type_of(current)
            if not entry:
                break
            opcode, ops = entry
            if opcode == OP_TYPE_ARRAY:
                length = self.constant_value(ops[2]) or 1
                count *= length
                current = ops[1]
            elif opcode == OP_TYPE_RUNTIME_ARRAY:
                current = ops[1]
                break
            else:
                break
        return current, count

    def descriptors(self):
        result = {}
        for _result_type, result_id, storage in self.variables:
            if storage not in (STORAGE_UNIFORM_CONSTANT, STORAGE_UNIFORM, STORAGE_STORAGE_BUFFER):
                continue
            pointer = self.type_of(_result_type)
            if not pointer or pointer[0] != OP_TYPE_POINTER:
                continue
            target = pointer[1][2]
            target, count = self.unwrap_array(target)
            entry = self.type_of(target)
            if not entry:
                continue
            opcode, ops = entry
            dim = 0
            flags = 0
            if opcode == OP_TYPE_SAMPLED_IMAGE:
                descriptor_type = DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                image_type_id = ops[1]
                dim, flags = self.image_info(image_type_id)
            elif opcode == OP_TYPE_IMAGE:
                sampled = ops[6] if len(ops) > 6 else 0
                descriptor_type = DESCRIPTOR_TYPE_STORAGE_IMAGE if sampled == 2 else DESCRIPTOR_TYPE_SAMPLED_IMAGE
                dim, flags = self.image_info(target)
            elif opcode == OP_TYPE_SAMPLER:
                descriptor_type = DESCRIPTOR_TYPE_SAMPLER
            elif opcode == OP_TYPE_STRUCT:
                descriptor_type = self.struct_buffer_kind(target, storage)
            elif opcode == OP_TYPE_ACCELERATION_STRUCTURE_KHR:
                descriptor_type = DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE
            else:
                continue
            set_id = self.decoration(result_id, DECORATION_DESCRIPTOR_SET)
            binding = self.decoration(result_id, DECORATION_BINDING)
            if set_id is None or binding is None:
                continue
            key = (set_id[0], binding[0])
            result.setdefault(key, (descriptor_type, count, dim, flags))
        return result

    def push_constant_size(self):
        size = 0
        for result_type, _result_id, storage in self.variables:
            if storage != STORAGE_PUSH_CONSTANT:
                continue
            pointer = self.type_of(result_type)
            if not pointer or pointer[0] != OP_TYPE_POINTER:
                continue
            size = max(size, self.type_size(pointer[1][2]))
        return size

    def specialization_constants(self):
        result = []
        for target, decorations in self.decorations.items():
            if DECORATION_SPEC_ID not in decorations:
                continue
            entry = self.constants.get(target)
            if not entry:
                continue
            opcode, type_id, _values = entry
            if opcode not in (OP_SPEC_CONSTANT, OP_SPEC_CONSTANT_TRUE, OP_SPEC_CONSTANT_FALSE,
                              OP_SPEC_CONSTANT_OP):
                continue
            type_entry = self.type_of(type_id)
            if not type_entry:
                continue
            type_opcode, _ = type_entry
            if type_opcode == OP_TYPE_BOOL:
                constant_type = SPEC_BOOL
            elif type_opcode == OP_TYPE_INT:
                constant_type = SPEC_UINT32 if type_entry[1][2] == 0 else SPEC_INT32
            elif type_opcode == OP_TYPE_FLOAT:
                constant_type = SPEC_FLOAT
            else:
                continue
            constant_id = decorations[DECORATION_SPEC_ID][0]
            if constant_id >= 256:
                continue
            result.append((constant_id, constant_type))
        result.sort(key=lambda item: item[0])
        return result

    def vertex_inputs(self):
        result = []
        for result_type, result_id, storage in self.variables:
            if storage != STORAGE_INPUT:
                continue
            if self.decoration(result_id, DECORATION_BUILT_IN) is not None:
                continue
            location = self.decoration(result_id, DECORATION_LOCATION)
            if location is None:
                continue
            pointer = self.type_of(result_type)
            if not pointer or pointer[0] != OP_TYPE_POINTER:
                continue
            entry = self.type_of(pointer[1][2])
            if not entry:
                continue
            opcode, ops = entry
            if opcode == OP_TYPE_VECTOR:
                base_id, vector_size = ops[1], ops[2]
            else:
                base_id, vector_size = pointer[1][2], 1
            base = self.type_of(base_id)
            if not base:
                continue
            base_opcode, base_ops = base
            fmt = 0
            if base_opcode == OP_TYPE_FLOAT:
                fmt = FLOAT_FORMAT_TABLE.get(vector_size, 0) if base_ops[1] == 32 else (
                    HALF_FORMAT_TABLE.get(vector_size, 0) if base_ops[1] == 16 else 0)
            elif base_opcode == OP_TYPE_INT:
                signed = bool(base_ops[2])
                fmt = FORMAT_TABLE.get((vector_size, base_ops[1], signed), 0)
            if fmt:
                result.append((location[0], fmt))
        result.sort(key=lambda item: item[0])
        return result


def build_lsb(spirv_words, stage_flags, push_size_override=None):
    spirv = Spirv(spirv_words)
    descriptors = spirv.descriptors()
    push_size = push_size_override if push_size_override is not None else spirv.push_constant_size()
    spec_constants = spirv.specialization_constants()
    inputs = spirv.vertex_inputs() if stage_flags == STAGE_VERTEX else []
    local_size = spirv.local_size if stage_flags == STAGE_COMPUTE else None

    push = b"\x01" + _u16(push_size) if push_size > 0 else b"\x00"

    spec = bytearray(_u32(len(spec_constants)))
    for constant_id, constant_type in spec_constants:
        spec += _u32(constant_id) + _u32(constant_type)

    sets = {}
    for (set_id, binding), value in descriptors.items():
        sets.setdefault(set_id, []).append((binding, value))
    descriptor_sets = bytearray(_u16(len(sets)))
    for set_id in sorted(sets):
        bindings = sorted(sets[set_id], key=lambda item: item[0])
        descriptor_sets += _u16(set_id) + _u16(len(bindings))
        for binding, (descriptor_type, count, dim, flags) in bindings:
            descriptor_sets += (_u16(binding) + _u16(descriptor_type) + _u16(count) +
                                struct.pack("<BB", dim & 0xFF, flags & 0xFF))

    vertex_inputs = bytearray(_u16(len(inputs)))
    for location, fmt in inputs:
        vertex_inputs += _u16(location) + _u16(fmt)

    local = b""
    if local_size:
        local = _u32(local_size[0]) + _u32(local_size[1]) + _u32(local_size[2])

    sections = [bytes(push), bytes(spec), bytes(descriptor_sets), bytes(vertex_inputs)]
    names = ["push", "spec", "sets", "inputs"]
    if local:
        sections.append(local)
        names.append("local")

    offsets = {"local": 0}
    offset = 16
    for name, section in zip(names, sections):
        offsets[name] = offset
        offset += len(section)

    header = (b"rfl\x01" + _u16(stage_flags) +
              _u16(offsets["push"]) +
              _u16(offsets["spec"]) +
              _u16(offsets["sets"]) +
              _u16(offsets["inputs"]) +
              _u16(offsets["local"]))
    return header + b"".join(sections)


def stage_flags_for_path(path):
    if path.endswith(".vert.spv"):
        return STAGE_VERTEX
    if path.endswith(".frag.spv"):
        return STAGE_FRAGMENT
    if path.endswith(".comp.spv"):
        return STAGE_COMPUTE
    return 0


def _read_json(path):
    import json
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_pipeline_push_sizes(repo):
    """Map `<stage>.spv` relative path -> pipeline layout push constant size.

    LumeGS ships an explicit `.shaderpl` next to every shader. Using its push
    constant size keeps the reflection layout compatible with the explicit
    layout that the render nodes actually bind. Without this override unused
    push constant blocks (e.g. the default 3D material lighting block) would be
    declared in the reflection but not in the pipeline layout.
    """
    sizes = {}
    assets = os.path.join(repo, "lume", "LumeGS", "assets", "app")
    shader_root = os.path.join(assets, "shaders")
    pl_root = os.path.join(assets, "pipelinelayouts")
    if not os.path.isdir(shader_root) or not os.path.isdir(pl_root):
        return sizes
    for dirpath, _dirs, names in os.walk(shader_root):
        for name in names:
            if not name.endswith(".shader"):
                continue
            try:
                shader_json = _read_json(os.path.join(dirpath, name))
            except (OSError, ValueError):
                continue
            for variant in shader_json.get("shaders", []):
                pl_uri = variant.get("pipelineLayout", "")
                pl_name = pl_uri.split("://", 1)[-1]
                pl_path = os.path.join(pl_root, pl_name)
                push_size = 0
                if pl_name and os.path.isfile(pl_path):
                    try:
                        pl_json = _read_json(pl_path)
                        push_size = int(pl_json.get("pushConstant", {}).get("size", 0))
                    except (OSError, ValueError, TypeError):
                        push_size = 0
                for key in ("vert", "frag", "comp", "compute"):
                    spv_uri = variant.get(key, "")
                    if not spv_uri.endswith(".spv"):
                        continue
                    rel = spv_uri.split("://", 1)[-1]
                    sizes.setdefault(rel, push_size)
    return sizes


def write_lsb(spv_path, push_size_override=None):
    stage_flags = stage_flags_for_path(spv_path)
    if not stage_flags:
        return
    with open(spv_path, "rb") as f:
        raw = f.read()
    if len(raw) % 4 != 0:
        raise SystemExit("SPIR-V size is not a multiple of 4: " + spv_path)
    words = list(struct.unpack("<%dI" % (len(raw) // 4), raw))
    if not words or words[0] != 0x07230203:
        raise SystemExit("Not a little-endian SPIR-V module: " + spv_path)
    blob = build_lsb(words, stage_flags, push_size_override)
    with open(spv_path + ".lsb", "wb") as f:
        f.write(blob)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..")))
    parser.add_argument("--spv", default=None)
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    spv_dir = os.path.abspath(args.spv) if args.spv else os.path.join(
        repo, "LumeApp", "entry", ".cxx", "gs_assets")
    push_sizes = load_pipeline_push_sizes(repo)
    count = 0
    for dirpath, _dirs, names in os.walk(spv_dir):
        for name in names:
            if not name.endswith(".spv"):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, spv_dir).replace(os.sep, "/")
            write_lsb(path, push_sizes.get(rel))
            count += 1
    print("generated %d lsb files under %s" % (count, spv_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
