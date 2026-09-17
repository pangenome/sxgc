import Lake
open Lake DSL

package sxgc

lean_lib Sxgc

@[default_target]
lean_exe sxgctest where
  root := `Main

lean_exe dbg where
  root := `Debug
