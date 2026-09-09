#!/usr/bin/env python3
"""
reflect_bindings.py — derive the descriptor set layout from the compiled SPIR-V.

The descriptor set layout must agree exactly with what the shaders declare.
Transcribing 38 bindings by hand from the HeapIndices enum invites a silent
mismatch: a wrong descriptor type or count renders incorrectly rather than
erroring, which is the worst failure mode available. So the table is generated
from the shaders themselves and cannot drift from them.

Reads spirv-dis output for each .spv and emits a C++ header.

RT64 does not use one shared descriptor table. The D3D12 original allocates
several heaps — descriptorHeap for the ray tracing passes, composeHeap,
postProcessHeap, samplerHeap and per-filter heaps — and the same register
number means different things in different passes. Binding 106 is a
SAMPLED_IMAGE in ComposePS but a STORAGE_BUFFER in DirectRayGen, so merging
everything into one set is unsatisfiable. Bindings are therefore grouped by
pass, one descriptor set layout each.

Usage: reflect_bindings.py <spv-dir> <output-header>
"""
import re
import subprocess
import sys
from pathlib import Path

# SPIR-V "Sampled" operand of OpTypeImage: 1 = read via sampler, 2 = storage.
SAMPLED_READ = "1"
SAMPLED_STORAGE = "2"

# SPIR-V image format -> VkFormat. Storage images must be created with the
# format the shader declares, so this is not cosmetic.
SPV_FORMAT_TO_VK = {
    "Rgba32f": "VK_FORMAT_R32G32B32A32_SFLOAT",
    "Rgba16f": "VK_FORMAT_R16G16B16A16_SFLOAT",
    "Rg32f":   "VK_FORMAT_R32G32_SFLOAT",
    "Rg16f":   "VK_FORMAT_R16G16_SFLOAT",
    "R32f":    "VK_FORMAT_R32_SFLOAT",
    "R16f":    "VK_FORMAT_R16_SFLOAT",
    "R32i":    "VK_FORMAT_R32_SINT",
    "R32ui":   "VK_FORMAT_R32_UINT",
    "Rgba8":   "VK_FORMAT_R8G8B8A8_UNORM",
    "Rgba8Snorm":  "VK_FORMAT_R8G8B8A8_SNORM",
    "Rgba16":      "VK_FORMAT_R16G16B16A16_UNORM",
    "Rgba16Snorm": "VK_FORMAT_R16G16B16A16_SNORM",
    "R16ui":       "VK_FORMAT_R16_UINT",
    "R16i":        "VK_FORMAT_R16_SINT",
    "Rg16f":       "VK_FORMAT_R16G16_SFLOAT",
    "Unknown": "VK_FORMAT_UNDEFINED",
}

STAGE_FROM_EXEC_MODEL = {
    "Vertex": "VK_SHADER_STAGE_VERTEX_BIT",
    "Fragment": "VK_SHADER_STAGE_FRAGMENT_BIT",
    "Geometry": "VK_SHADER_STAGE_GEOMETRY_BIT",
    "GLCompute": "VK_SHADER_STAGE_COMPUTE_BIT",
    "RayGenerationKHR": "VK_SHADER_STAGE_RAYGEN_BIT_KHR",
    "MissKHR": "VK_SHADER_STAGE_MISS_BIT_KHR",
    "ClosestHitKHR": "VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR",
    "AnyHitKHR": "VK_SHADER_STAGE_ANY_HIT_BIT_KHR",
    "IntersectionKHR": "VK_SHADER_STAGE_INTERSECTION_BIT_KHR",
    "CallableKHR": "VK_SHADER_STAGE_CALLABLE_BIT_KHR",
}


class Module:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.results = {}        # %id -> full instruction line
        self.decorations = {}    # %id -> {decoration: value}
        self.constants = {}      # %id -> int
        self.stages = set()
        self._index()

    def _index(self):
        for line in self.lines:
            stripped = line.strip()

            m = re.match(r"(%\w+)\s*=\s*(Op\w+)(.*)", stripped)
            if m:
                self.results[m.group(1)] = (m.group(2), m.group(3).strip())
                if m.group(2) in ("OpConstant",):
                    parts = m.group(3).split()
                    if parts and parts[-1].isdigit():
                        self.constants[m.group(1)] = int(parts[-1])

            m = re.match(r"OpDecorate\s+(%\w+)\s+(\w+)\s*(\d+)?", stripped)
            if m:
                self.decorations.setdefault(m.group(1), {})[m.group(2)] = m.group(3)

            m = re.match(r"OpEntryPoint\s+(\w+)\s", stripped)
            if m:
                self.stages.add(m.group(1))

    def descriptor_of(self, var_id):
        """Return (vk_descriptor_type, count) for a decorated variable."""
        entry = self.results.get(var_id)
        if entry is None or entry[0] != "OpVariable":
            return None, 1
        # OpVariable %ptrType StorageClass
        parts = entry[1].split()
        if not parts:
            return None, 1
        ptr_id = parts[0]
        storage_class = parts[1] if len(parts) > 1 else ""

        ptr = self.results.get(ptr_id)
        if ptr is None or ptr[0] != "OpTypePointer":
            return None, 1
        inner = ptr[1].split()
        pointee = inner[1] if len(inner) > 1 else None

        return self._type_to_descriptor(pointee, storage_class)

    def image_format(self, var_id):
        """Declared storage format of an image variable, or UNDEFINED."""
        entry = self.results.get(var_id)
        if entry is None or entry[0] != "OpVariable":
            return "VK_FORMAT_UNDEFINED"
        ptr = self.results.get(entry[1].split()[0])
        if ptr is None:
            return "VK_FORMAT_UNDEFINED"
        type_id = ptr[1].split()[1] if len(ptr[1].split()) > 1 else None
        seen = 0
        while type_id and seen < 8:
            seen += 1
            t = self.results.get(type_id)
            if t is None:
                break
            if t[0] == "OpTypeArray":
                type_id = t[1].split()[0]
                continue
            if t[0] == "OpTypeImage":
                parts = t[1].split()
                if len(parts) >= 7:
                    return SPV_FORMAT_TO_VK.get(parts[6], "VK_FORMAT_UNDEFINED")
            break
        return "VK_FORMAT_UNDEFINED"

    def _type_to_descriptor(self, type_id, storage_class, count=1):
        entry = self.results.get(type_id)
        if entry is None:
            return None, count
        op, operands = entry
        parts = operands.split()

        if op == "OpTypeArray":
            elem = parts[0]
            length_id = parts[1] if len(parts) > 1 else None
            n = self.constants.get(length_id, 1)
            return self._type_to_descriptor(elem, storage_class, n)

        if op == "OpTypeRuntimeArray":
            return self._type_to_descriptor(parts[0], storage_class, 0)

        if op == "OpTypeAccelerationStructureKHR":
            return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR", count

        if op == "OpTypeSampler":
            return "VK_DESCRIPTOR_TYPE_SAMPLER", count

        if op == "OpTypeSampledImage":
            return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", count

        if op == "OpTypeImage":
            # sampled type, Dim, Depth, Arrayed, MS, Sampled, Format
            if len(parts) >= 6:
                dim = parts[1]
                sampled = parts[5]
                # Dim=Buffer is a texel buffer, not an image. RT64 declares
                # gHitColor and friends as RWBuffer<>, which reaches here as
                # Buffer/Sampled=2 — typing those as STORAGE_IMAGE produces a
                # descriptor mismatch that renders garbage rather than erroring.
                if dim == "Buffer":
                    if sampled == SAMPLED_STORAGE:
                        return "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER", count
                    return "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER", count
                if sampled == SAMPLED_STORAGE:
                    return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE", count
                if sampled == SAMPLED_READ:
                    return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE", count
            return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE", count

        if op == "OpTypeStruct":
            if storage_class == "StorageBuffer":
                return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER", count
            return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER", count

        return None, count


def reflect(spv_path):
    """Yield (set, binding, type, count, stages, name) for one module."""
    try:
        text = subprocess.run(["spirv-dis", str(spv_path)],
                              capture_output=True, text=True,
                              check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        raise SystemExit(f"spirv-dis failed on {spv_path}: {exc}")

    module = Module(text)
    stages = {STAGE_FROM_EXEC_MODEL[s] for s in module.stages
              if s in STAGE_FROM_EXEC_MODEL}

    names = {}
    for line in module.lines:
        m = re.match(r'\s*OpName\s+(%\w+)\s+"([^"]*)"', line)
        if m:
            names[m.group(1)] = m.group(2)

    for var_id, decs in module.decorations.items():
        if "Binding" not in decs:
            continue
        binding = int(decs["Binding"])
        dset = int(decs.get("DescriptorSet") or 0)
        dtype, count = module.descriptor_of(var_id)
        if dtype is None:
            continue
        yield (dset, binding, dtype, count, stages,
               names.get(var_id, var_id), module.image_format(var_id))


# One group per D3D12 heap. Shaders in a group share a descriptor set layout,
# so their bindings must agree; shaders in different groups need not.
#
# Membership is by EXACT NAME, never a pattern. A predicate like
# name.endswith("RayGen") silently swallows any shader that happens to match —
# a test shader called TestRayGen joined the RayTracing group and, because its
# gOutput sorts before gViewDirection, renamed binding 0. Explicit lists make
# that impossible, and an unrecognised shader is an error rather than a silent
# merge into whichever group its name resembles.
GROUP_MEMBERS = {
    "RayTracing":   ["PrimaryRayGen", "DirectRayGen", "IndirectRayGen",
                     "ReflectionRayGen", "RefractionRayGen"],
    "Compose":      ["ComposePS"],
    "PostProcess":  ["PostProcessPS"],
    "Debug":        ["DebugPS"],
    "Gaussian":     ["GaussianFilterRGB3x3CS"],
    "GenerateMips": ["GenerateMipsCS"],
    "Im3D":         ["Im3DVS", "Im3DPS", "Im3DGSLines", "Im3DGSPoints"],
}

# Shaders that legitimately declare no descriptors.
NO_BINDINGS = ["FullScreenVS"]


def merge_group(spv_files):
    merged = {}     # (set, binding) -> dict
    for spv in spv_files:
        for dset, binding, dtype, count, stages, name, fmt in reflect(spv):
            key = (dset, binding)
            existing = merged.get(key)
            if existing is None:
                merged[key] = {"type": dtype, "count": count,
                               "stages": set(stages), "names": {name},
                               "shaders": {spv.stem}, "format": fmt}
                continue
            # Same binding used by several shaders: types must agree, or the
            # layout is unsatisfiable and we should say so loudly.
            if existing["type"] != dtype:
                raise SystemExit(
                    f"conflicting descriptor types at set {dset} binding "
                    f"{binding}: {existing['type']} (from "
                    f"{sorted(existing['shaders'])}) vs {dtype} (from "
                    f"{spv.stem}). These shaders cannot share a descriptor "
                    f"set layout — check the GROUPS table.")
            if fmt != "VK_FORMAT_UNDEFINED":
                existing["format"] = fmt
            existing["count"] = max(existing["count"], count)
            existing["stages"] |= stages
            existing["names"].add(name)
            existing["shaders"].add(spv.stem)
    return merged


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    spv_dir, out_path = Path(sys.argv[1]), Path(sys.argv[2])

    all_spv = sorted(spv_dir.glob("*.spv"))
    by_stem = {f.stem: f for f in all_spv}

    # An unexpected .spv in this directory means either a new shader that needs
    # a group, or a stray file that would corrupt an existing one. Either way,
    # say so rather than guessing.
    known = set(NO_BINDINGS)
    for members in GROUP_MEMBERS.values():
        known.update(members)
    strays = sorted(set(by_stem) - known)
    if strays:
        raise SystemExit(
            "unrecognised shader(s) in " + str(spv_dir) + ": " +
            ", ".join(strays) + "\nAdd them to GROUP_MEMBERS (or NO_BINDINGS) "
            "in reflect_bindings.py. Test shaders belong in a separate output "
            "directory, not here.")

    groups = []
    for name in GROUP_MEMBERS:
        files = [by_stem[m] for m in GROUP_MEMBERS[name] if m in by_stem]
        missing = [m for m in GROUP_MEMBERS[name] if m not in by_stem]
        if missing:
            raise SystemExit(
                f"group {name} expects {missing} but they were not compiled")
        if not files:
            continue
        groups.append((name, files, merge_group(files)))

    uncovered = sorted(m for m in NO_BINDINGS if m in by_stem)

    lines = [
        "/* Generated by tools/reflect_bindings.py — do not edit.",
        " *",
        " * Descriptor set layouts derived from the compiled SPIR-V, so they",
        " * cannot drift from what the shaders declare.",
        " *",
        " * One group per pass, mirroring the separate descriptor heaps the",
        " * D3D12 original allocated. The same register number means different",
        " * things in different passes, so a single shared set is not possible.",
        " */",
        "#ifndef RT64_SHADER_BINDINGS_H",
        "#define RT64_SHADER_BINDINGS_H",
        "",
        "#include <vulkan/vulkan.h>",
        "",
        "namespace RT64 {",
        "",
        "struct ShaderBinding {",
        "    uint32_t set;",
        "    uint32_t binding;",
        "    VkDescriptorType type;",
        "    uint32_t count;",
        "    VkShaderStageFlags stages;",
        "    const char *name;",
        "    VkFormat format;   /* declared storage format, or UNDEFINED */",
        "};",
        "",
    ]

    for name, files, merged in groups:
        shaders = ", ".join(sorted(f.stem for f in files))
        lines.append(f"/* {name}: {shaders} */")
        lines.append(f"static const ShaderBinding kBindings{name}[] = {{")
        for (dset, binding) in sorted(merged):
            info = merged[(dset, binding)]
            stages = " | ".join(sorted(info["stages"])) or "0"
            vname = sorted(info["names"])[0]
            lines.append(f"    {{ {dset}, {binding}, {info['type']}, "
                         f"{info['count']}, {stages}, \"{vname}\", "
                         f"{info.get('format', 'VK_FORMAT_UNDEFINED')} }},")
        lines.append("};")
        lines.append(f"static const uint32_t kBindingCount{name} = "
                     f"{len(merged)};")
        lines.append("")

    lines.append("struct ShaderBindingGroup {")
    lines.append("    const char *name;")
    lines.append("    const ShaderBinding *bindings;")
    lines.append("    uint32_t count;")
    lines.append("};")
    lines.append("")
    lines.append("static const ShaderBindingGroup kShaderBindingGroups[] = {")
    for name, _files, _merged in groups:
        lines.append(f"    {{ \"{name}\", kBindings{name}, "
                     f"kBindingCount{name} }},")
    lines.append("};")
    lines.append(f"static const uint32_t kShaderBindingGroupCount = "
                 f"{len(groups)};")
    lines += [
        "",
        "} /* namespace RT64 */",
        "",
        "#endif /* RT64_SHADER_BINDINGS_H */",
        "",
    ]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))
    for name, files, merged in groups:
        print(f"  {name:<13} {len(merged):>3} bindings from "
              f"{', '.join(sorted(f.stem for f in files))}")
    if uncovered:
        print(f"  (no bindings / ungrouped: {', '.join(uncovered)})")


if __name__ == "__main__":
    main()
