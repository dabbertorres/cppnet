# net for C++

Networking library for C++. Uses C++ 20 features.

## Building (without tests)

```sh
cmake -B build -DBUILD_TESTING=OFF
cmake --build build
```

## Testing

Requires Catch2 v2.x (obtaining a version is currently left to the user)

```sh
cmake -B build
cmake --build build
ctest --test-dir build # OR directly run ./build/test/Debug/tests
```
