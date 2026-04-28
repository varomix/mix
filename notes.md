  Quick reference

  make              # build the compiler
  make test         # run all tests
  ./build/mix file.mix -o out && ./out   # compile + run
  ./build/mix file.mix --emit-tokens     # debug: tokens
  ./build/mix file.mix --emit-ast        # debug: AST
  ./build/mix file.mix --emit-ir -o f.ssa  # debug: QBE IR


fruits = ["orange", "apple", "pineapple"]
// should print the fruits list
print("las frutas {fruits}")