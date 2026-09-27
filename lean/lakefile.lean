import Lake
open Lake DSL

package sxgc

lean_lib Sxgc
lean_lib SxgcBuild

@[default_target]
lean_exe sxgctest where
  root := `Main

lean_exe dbg where
  root := `Debug
