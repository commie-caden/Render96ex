file(REMOVE_RECURSE
  "CMakeFiles/rt64_shaders"
  "shaders/ComposePS.spv"
  "shaders/DebugPS.spv"
  "shaders/DirectRayGen.spv"
  "shaders/FullScreenVS.spv"
  "shaders/GaussianFilterRGB3x3CS.spv"
  "shaders/GenerateMipsCS.spv"
  "shaders/Im3DGSLines.spv"
  "shaders/Im3DGSPoints.spv"
  "shaders/Im3DPS.spv"
  "shaders/Im3DVS.spv"
  "shaders/IndirectRayGen.spv"
  "shaders/PostProcessPS.spv"
  "shaders/PrimaryRayGen.spv"
  "shaders/ReflectionRayGen.spv"
  "shaders/RefractionRayGen.spv"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/rt64_shaders.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
