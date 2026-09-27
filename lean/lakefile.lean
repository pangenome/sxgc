import Lake
open Lake DSL

package sxgc

lean_lib Sxgc
lean_lib SxgcBuild
lean_lib SxgcBounds
lean_lib SxgcPhi

@[default_target]
lean_exe sxgctest where
  root := `Main

lean_exe dbg where
  root := `Debug
