"""Lower the FFP's unclamped SPIR-V LOD queries to WebGPU derivative math.

WGSL has no textureQueryLod. Only the unclamped component is used by these
shaders. Reject other consumers instead of inventing sampler clamp semantics.
The mip footprint is log2(max(length(dUVdx * size), length(dUVdy * size))).
Cube directions are projected onto their major-axis face before differentiation.
As with native implicit sampling, derivatives at cube-face boundaries are not
defined; this is not a promise of bit-identical GPU texture filtering.
"""
import struct


def lower(code: bytes, grammar: dict) -> tuple[bytes, int]:
    words = list(struct.unpack(f"<{len(code) // 4}I", code))
    assert words[0] == 0x07230203
    instructions, cursor = [], 5
    while cursor < len(words):
        size, op = words[cursor] >> 16, words[cursor] & 65535
        assert size > 0 and cursor + size <= len(words)
        instructions.append((op, words[cursor + 1:cursor + size]))
        cursor += size
    queries = {a[1] for op, a in instructions if op == 105}
    if not queries:
        return code, 0
    schema = {row['opcode']: row for row in grammar['instructions']}
    types, value_types = {}, {}
    glsl = None
    for op, a in instructions:
        if 19 <= op <= 39:
            types[a[0]] = (op, a[1:])
        roles = schema[op].get('operands', [])
        if len(roles) >= 2 and roles[0]['kind'] == 'IdResultType':
            value_types[a[1]] = a[0]
        if op == 11 and struct.pack(f'<{len(a)-1}I', *a[1:]).split(b'\0')[0] == b'GLSL.std.450':
            glsl = a[0]
        # Queries in the checked-in FFP pack have only Extract(..., 1) uses.
        for k, role in enumerate(roles):
            if role['kind'] == 'IdRef' and k < len(a) and a[k] in queries:
                assert op == 81 and a[2] in queries and a[3:] == [1], \
                    'WebGPU cannot preserve a clamped LOD query without sampler state'
    assert glsl is not None
    next_id = words[3]
    globals_, output = [], []

    def fresh():
        nonlocal next_id
        result = next_id
        next_id += 1
        return result

    def type_id(op, *args):
        for identity, (kind, data) in types.items():
            if kind == op and data == list(args):
                return identity
        identity = fresh()
        types[identity] = (op, list(args))
        globals_.append((op, [identity, *args]))
        return identity

    def constant(t, bits):
        for op, a in instructions + globals_:
            if op == 43 and a[0] == t and a[2:] == [bits]:
                return a[1]
        identity = fresh()
        globals_.append((43, [t, identity, bits]))
        return identity

    def emit(op, t, *args):
        identity = fresh()
        output.append((op, [t, identity, *args]))
        return identity

    for op, a in instructions:
        if op != 105:
            output.append((op, a))
            continue
        result_type, result, sampled, coord = a
        assert types[result_type][0] == 23 and types[result_type][1][1] == 2
        f32 = types[result_type][1][0]
        assert types[f32] == (22, [32])
        sampled_type = value_types[sampled]
        assert types[sampled_type][0] == 27
        image_type = types[sampled_type][1][0]
        image_args = types[image_type][1]
        dimension, arrayed, multisampled = image_args[1], image_args[3], image_args[4]
        assert dimension in (1, 2, 3) and not arrayed and not multisampled
        count = 3 if dimension == 2 else 2  # SPIR-V: 2D=1, 3D=2, Cube=3
        vector = type_id(23, f32, count)
        i32 = type_id(21, 32, 1)
        ivec = type_id(23, i32, count)
        zero = constant(i32, 0)
        half = constant(f32, 0x3F000000)
        image = emit(100, image_type, sampled)
        size_i = emit(103, ivec, image, zero)
        size_f = emit(111, vector, size_i)
        uv = coord
        if dimension == 3:
            scalar = [emit(81, f32, coord, index) for index in range(3)]
            absolute = [emit(12, f32, glsl, 4, value) for value in scalar]  # FAbs
            x, y, z = scalar
            ax, ay, az = absolute
            boolean = type_id(20)
            major_x = emit(167, boolean,
                           emit(190, boolean, ax, ay), emit(190, boolean, ax, az))
            major_y = emit(190, boolean, ay, az)
            # Sign changes and +0.5 offsets disappear from derivative length.
            uv_x = emit(80, vector, emit(136, f32, z, ax), emit(136, f32, y, ax))
            uv_y = emit(80, vector, emit(136, f32, x, ay), emit(136, f32, z, ay))
            uv_z = emit(80, vector, emit(136, f32, x, az), emit(136, f32, y, az))
            # OpSelect's vector condition must have the same component count.
            bvec = type_id(23, boolean, 2)
            bx = emit(80, bvec, major_x, major_x)
            by = emit(80, bvec, major_y, major_y)
            uv = emit(142, vector, emit(169, vector, bx, uv_x,
                         emit(169, vector, by, uv_y, uv_z)), half)
        dx = emit(133, vector, emit(207, vector, uv), size_f)
        dy = emit(133, vector, emit(208, vector, uv), size_f)
        rho2 = emit(12, f32, glsl, 40, emit(148, f32, dx, dx), emit(148, f32, dy, dy))
        lod = emit(133, f32, emit(12, f32, glsl, 30, rho2), half)  # Log2
        # Component 0 is provably unused (checked above).
        output.append((80, [result_type, result, lod, lod]))
    result_words = words[:5]
    result_words[3] = next_id
    inserted = False
    for op, a in output:
        if op == 54 and not inserted:  # global declarations before first function
            for kind, args in globals_:
                result_words.extend([(len(args) + 1) << 16 | kind, *args])
            inserted = True
        result_words.extend([(len(a) + 1) << 16 | op, *a])
    assert inserted
    return struct.pack(f'<{len(result_words)}I', *result_words), len(queries)
