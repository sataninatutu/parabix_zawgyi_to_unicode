This project implements a high-performance Zawgyi-to-Unicode transcoder using the Parabix parallel bitstream framework. Zawgyi is a legacy encoding for the Myanmar script that remains widely used despite being non-standard and incompatible with Unicode. 
The converter processes text through a multi-stage pipeline: UTF-8 decoding, character classification, 1-to-1 mapping (Category 1), 1-to-multiple expansion (Category 2), and UTF-8 re-encoding—all operating on parallel bit streams using SIMD instructions.


Prerequisites:
- Parabix Framework (the converter is built as a tool within the Parabix ecosystem)
- LLVM (for JIT compilation)
- CMake 3.10+
- C++14 compatible compiler


From https://cs-git-research.cs.sfu.ca/cameron/parabix-devel/-/tree/master/,


parabix-devel/tools/ranscoders/zawgyi2unicode.cpp   ← Main converter file

1. Go to build folder: 
cd ~/parabix-devel/build

2. Make using CMake: 
make zawgyi2unicode

3. Basic Usage: 
./bin/zawgyi2unicode <input_file.zawgyi>

4. Converting: 
./bin/zawgyi2unicode my_zawgyi_file.txt > unicode_output.txt


Note: The CMakeLists within the transcoder file will also have to be modified. 

