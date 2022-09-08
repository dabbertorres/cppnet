# net for C++

Networking library for C++. Uses C++ 20 features.

## Building

Uses vcpkg for obtaining dependencies.

```sh
cmake -B build \
  -DBUILD_TESTING=TRUE \
  -DBUILD_EXAMPLE=TRUE \
cmake --build build
```

### Examples

#### Specify Compiler and Alternate Build System

```sh
cmake -B build \
  -G"Ninja Multi-Config" \
  -DBUILD_TESTING=TRUE \
  -DBUILD_EXAMPLE=TRUE \
  -DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++ \
  -DCMAKE_PREFIX_PATH=/usr/local/opt/llvm/lib
cmake --build build --config Debug
```

## Testing

Uses Catch2.

```sh
cmake -B build -DBUILD_TESTING=TRUE
cmake --build build
ctest --test-dir build # OR directly run ./build/test/Debug/tests
```
