file(READ "${INPUT}" _surface_ptx)
file(WRITE "${OUTPUT}" "#pragma once\ninline constexpr char surfaceCudaPtx[]=R\"SURFACE_PTX(${_surface_ptx})SURFACE_PTX\";\n")
