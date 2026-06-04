# SA3D C# to C++20 Port Implementation Plan

## Purpose

This document is the implementation guide for porting the attached SA3D C# modeling support files to native C++20 while preserving as much behavior as practical. The target style is native C++ rather than a mechanical C# transcription: value types, `enum class`, explicit getters/setters where C# used properties, free functions where C# used extension methods, templated callables where C# used delegates, and `std::vector`/`std::span` for collection views.

The port should live under the existing namespace:

```cpp
namespace Sa3Dport::Structs {
    // ...
}
```

## Final decisions

| Topic | Decision |
|---|---|
| C++ version | C++20. Use `std::span`, `std::endian`, defaulted comparisons where useful, and `constexpr` helpers. |
| Floating-point rounding | Reproduce .NET `MathF.Round` default behavior for now, meaning midpoint values round to nearest even. Do not use plain `std::round` for compatibility-critical serialization. |
| Matrix convention | Implement a custom `Matrix4x4` whose fields and multiplication behavior match C# `System.Numerics.Matrix4x4` usage in the source files. |
| LUT identity | Use pointer/reference identity as the default write-side collection identity. Also provide optional value-based deduplication functionality. |
| Namespace | Keep `Sa3Dport::Structs`, matching the existing `BAMSFHelper.h`. |
| Math layer | Implement and use a custom math layer. Do not depend on GLM, Qt math types, or DirectXMath for this port. |
| `Bounds` behavior | Preserve the exact C# API behavior: position/radius mutation must update the cached matrix. |
| Endian I/O | Implement `EndianStackReader` and `EndianStackWriter` as part of this port. |

## Source files covered

| C# / Existing File | Role in the port |
|---|---|
| `ColorIOType.cs` | Color storage format enum and byte-size helper. |
| `FloatIOType.cs` | Float/short/int/BAMS/BAMSF read, write, print, and byte-size behavior. |
| `Color.cs` | RGBA value type, packed color conversions, hex parsing, arithmetic, equality. |
| `EndianIOExtensions.cs` | Read/write helpers for colors, vectors, quaternions, arrays, collections, LUT-backed values, and labeled arrays. |
| `VectorUtilities.cs` | Vector helpers such as average, bounding center, normal/euler conversions, approximate distance. |
| `MatrixUtilities.cs` | Rotation matrices, transform matrices, normal matrix, Euler extraction, compatible Euler selection. |
| `QuaternionUtilities.cs` | Quaternion-to-Euler, Euler-to-quaternion, compatible Euler, raw component lerp. |
| `Bounds.cs` | Bounding sphere value with cached transform matrix and endian read/write. |
| `PositionNormal.cs` | Exact position+normal key type with equality/hash behavior. |
| `BAMSFHelper.h` | Existing C++ helper to reuse/extend for BAMSF conversion. |

## Proposed repository layout

```text
Sa3Dport/
  Structs/
    Math/
      Vector2.h
      Vector3.h
      Vector4.h
      Quaternion.h
      Matrix4x4.h
      MathHelper.h
      MathCompat.h
    IO/
      Endian.h
      EndianStackReader.h
      EndianStackWriter.h
    Lookup/
      BaseLUT.h
      LabeledArray.h
      LabeledReadOnlyArray.h
    BAMSFHelper.h
    Color.h
    ColorIOType.h
    FloatIOType.h
    EndianIOExtensions.h
    VectorUtilities.h
    MatrixUtilities.h
    QuaternionUtilities.h
    Bounds.h
    PositionNormal.h
```

Header-only is acceptable for small value types and templates. Larger math routines, matrix inversion, string formatting, and parsing can go in `.cpp` files if the project prefers faster rebuilds.

---

# Phase 1 - Math compatibility foundation

## 1.1 `MathCompat.h`

Create compatibility helpers that make C++ behavior match the C# implementation where required.

Required helpers:

```cpp
namespace Sa3Dport::Structs::MathCompat {
    float clamp01(float value);
    float round_to_even(float value);
    std::int16_t round_to_even_i16(float value);
    std::int32_t round_to_even_i32(float value);
    std::string fixed5_float(float value); // invariant-culture equivalent: "1.23456f"
}
```

Implementation notes:

- `round_to_even` should reproduce .NET `MathF.Round(value)` default midpoint behavior.
- Preserve NaN/infinity behavior only as far as needed by existing assets. Serialization inputs should normally be finite.
- `fixed5_float` should use a classic/invariant locale and append `f`, matching the C# `FloatIOType.Float` printer behavior.

Acceptance tests:

- `round_to_even(0.5f) == 0.0f`
- `round_to_even(1.5f) == 2.0f`
- `round_to_even(2.5f) == 2.0f`
- `round_to_even(-0.5f) == 0.0f`
- `round_to_even(-1.5f) == -2.0f`

## 1.2 `Vector2`, `Vector3`, `Vector4`

Implement compact value structs with public lowercase fields.

```cpp
struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(float x_, float y_);

    float& operator[](std::size_t index);
    const float& operator[](std::size_t index) const;
};
```

`Vector3` and `Vector4` follow the same pattern.

Required operations:

- `operator+`, `operator-`, unary `-`.
- Scalar `operator*`, `operator/`.
- Compound assignment variants where useful.
- Exact `operator==` and `operator!=`.
- `dot` for `Vector2`, `Vector3`, and `Vector4`.
- `length_squared`, `length`, `distance`.
- `normalize` if needed by quaternion/matrix implementation.
- Static constants or factory functions: zero, unit axes.

C# compatibility notes:

- C# `Vector3` equality is exact component equality. Keep exact equality for default operators.
- Approximate comparisons belong in `VectorUtilities`, not in the base vector type.
- `operator[]` must support the Euler compatibility loops from `MatrixUtilities`.

## 1.3 `Quaternion`

Implement:

```cpp
struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quaternion() = default;
    constexpr Quaternion(float x_, float y_, float z_, float w_);
};
```

Required operations:

- Exact equality.
- `length_squared`, `length`, `normalize`.
- `create_from_rotation_matrix(const Matrix4x4&)`.

C# compatibility notes:

- The in-memory order is `x, y, z, w`.
- The endian serialized order is `w, x, y, z`; this is handled in `EndianIOExtensions`.

## 1.4 `Matrix4x4`

Implement a custom 4x4 matrix with C#-style field names or lowercase equivalents.

Recommended field names:

```cpp
struct Matrix4x4 {
    float m11 = 0, m12 = 0, m13 = 0, m14 = 0;
    float m21 = 0, m22 = 0, m23 = 0, m24 = 0;
    float m31 = 0, m32 = 0, m33 = 0, m34 = 0;
    float m41 = 0, m42 = 0, m43 = 0, m44 = 0;
};
```

Required functions:

```cpp
Matrix4x4 identity();
Matrix4x4 create_scale(float scale);
Matrix4x4 create_scale(Vector3 scale);
Matrix4x4 create_translation(Vector3 position);
Matrix4x4 create_from_quaternion(Quaternion q);
Matrix4x4 transpose(const Matrix4x4& m);
bool invert(const Matrix4x4& input, Matrix4x4& output);
Matrix4x4 operator*(const Matrix4x4& lhs, const Matrix4x4& rhs);
```

Compatibility rules:

- Multiplication must match how `System.Numerics.Matrix4x4` is being used by the C# code.
- Translation must occupy the same effective row/fields as C# `Matrix4x4.CreateTranslation` for the expression `scale * rotation * translation` to produce matching output.
- Initial matrices created by manual field initialization should default unspecified fields to `0.0f`, except functions that intentionally create identity matrices.
- Matrix inversion should return `false` rather than throwing. Callers such as `get_normal_matrix` can throw after `invert` fails.

Acceptance tests:

- Identity multiplication preserves both operands.
- `create_scale(scale) * create_translation(pos)` matches equivalent C# output for representative inputs.
- `create_from_quaternion(identity quaternion)` returns identity.
- `invert(identity)` returns identity.

## 1.5 `MathHelper.h`

Create the missing C# `MathHelper` equivalent.

Required constants and functions:

```cpp
namespace Sa3Dport::Structs::MathHelper {
    inline constexpr float Pi = 3.14159265358979323846f;
    inline constexpr float Tau = 2.0f * Pi;
    inline constexpr float HalfPi = 0.5f * Pi;

    std::int32_t rad_to_bams(float radians);      // 360 degrees = 0x10000
    float bams_to_rad(std::int32_t bams);

    std::string to_c_hex(std::uint16_t value);
    std::string to_c_hex(std::uint32_t value);
}
```

Compatibility notes:

- `FloatIOType.BAMS16` and `BAMS32` use this helper.
- `to_c_hex` should match the source intent of `ToCHex()`. Use a stable uppercase C-style hex format, such as `0xFFFF` / `0xFFFFFFFF`, unless existing project conventions require otherwise.

## 1.6 Existing `BAMSFHelper.h`

Reuse the attached `BAMSFHelper.h` in `Sa3Dport::Structs`.

Current helper provides:

- `kPi`
- `kTwoPi`
- `kBamsScale`
- `DegreesToBams`
- `BamsToDegrees`
- `RadiansToBams`
- `BamsToRadians`

Add compatibility wrappers if useful:

```cpp
class BAMSFHelper {
public:
    static std::int32_t RadToBAMSF(float radians);
    static float BAMSFToRad(std::int32_t value);
};
```

Important note: the existing `RadiansToBams` returns `float`. The C# call sites write the result as integer types. Add integer-returning wrappers to keep serialization call sites clear.

---

# Phase 2 - Endian I/O foundation

## 2.1 `Endian.h`

Implement:

```cpp
enum class Endian {
    Little,
    Big
};
```

Provide byte-swap/read/write primitives:

```cpp
std::uint16_t byteswap(std::uint16_t value);
std::uint32_t byteswap(std::uint32_t value);
std::int16_t byteswap(std::int16_t value);
std::int32_t byteswap(std::int32_t value);
float byteswap_float(float value);
```

Use `std::endian` to detect native endian where available. Since this is C++20, prefer `std::endian::native`.

## 2.2 `EndianStackReader`

Implement an address-based reader over immutable bytes.

```cpp
class EndianStackReader {
public:
    EndianStackReader(std::span<const std::byte> data, Endian endian);

    std::uint8_t  read_u8(std::uint32_t address) const;
    std::int8_t   read_i8(std::uint32_t address) const;
    std::uint16_t read_u16(std::uint32_t address) const;
    std::int16_t  read_i16(std::uint32_t address) const;
    std::uint32_t read_u32(std::uint32_t address) const;
    std::int32_t  read_i32(std::uint32_t address) const;
    float         read_float(std::uint32_t address) const;

    Endian endian() const;
    std::size_t size() const;
};
```

Compatibility behavior:

- Reads do not change internal position.
- Address-advancing behavior is implemented by helper functions taking `std::uint32_t& address`.
- Out-of-range reads throw `std::out_of_range`.
- Floats should be read by copying bytes into `std::uint32_t`, applying endian conversion, then `std::bit_cast<float>`.

Optional C#-name aliases may be added if it improves port readability:

```cpp
std::uint32_t ReadUInt(std::uint32_t address) const;
float ReadFloat(std::uint32_t address) const;
```

But the primary C++ API should use snake_case.

## 2.3 `EndianStackWriter`

Implement an append-only writer with a pointer-position equivalent.

```cpp
class EndianStackWriter {
public:
    explicit EndianStackWriter(Endian endian);

    std::uint32_t pointer_position() const;

    void write_u8(std::uint8_t value);
    void write_i8(std::int8_t value);
    void write_u16(std::uint16_t value);
    void write_i16(std::int16_t value);
    void write_u32(std::uint32_t value);
    void write_i32(std::int32_t value);
    void write_float(float value);

    Endian endian() const;
    const std::vector<std::byte>& data() const;
    std::vector<std::byte> take_data();
};
```

Compatibility behavior:

- `pointer_position()` is the current byte offset and replaces C# `PointerPosition`.
- Writes append to the current end of the buffer.
- No implicit alignment or padding.
- Floats are written via `std::bit_cast<std::uint32_t>` followed by endian-aware integer write.

---

# Phase 3 - Core enums and scalar conversion

## 3.1 `ColorIOType.h`

Implement:

```cpp
enum class ColorIOType {
    ARGB8_32,
    ARGB8_16,
    ARGB4,
    RGB565,
    RGBA8
};

constexpr int byte_size(ColorIOType type);
```

Behavior:

| Type | Size |
|---|---:|
| `ARGB8_32` | 4 |
| `ARGB8_16` | 4 |
| `ARGB4` | 2 |
| `RGB565` | 2 |
| `RGBA8` | 4 |

Invalid enum values should either return `0` if preserving the C# helper literally or throw/assert if stronger C++ behavior is preferred. Prefer throwing `std::invalid_argument` for non-`constexpr` paths and using `default: return 0` only in the `constexpr` helper if necessary.

## 3.2 `FloatIOType.h`

Implement:

```cpp
enum class FloatIOType {
    Float,
    Short,
    Integer,
    BAMS16,
    BAMS32,
    BAMS16F,
    BAMS32F
};

constexpr int byte_size(FloatIOType type);
std::string print_float_as(float value, FloatIOType type);
void write_float_as(EndianStackWriter& writer, float value, FloatIOType type);
float read_float_as(const EndianStackReader& reader, std::uint32_t address, FloatIOType type);
```

Behavior table:

| Type | Bytes | Write | Read | Print |
|---|---:|---|---|---|
| `Float` | 4 | raw float | raw float | fixed 5 decimals + `f` |
| `Short` | 2 | .NET-rounded `int16_t` | `int16_t` converted to float | rounded integer text |
| `Integer` | 4 | .NET-rounded `int32_t` | `int32_t` converted to float | rounded integer text |
| `BAMS16` | 2 | `MathHelper::rad_to_bams` cast to `int16_t` | `MathHelper::bams_to_rad(read_i16)` | C hex of `uint16_t` |
| `BAMS32` | 4 | `MathHelper::rad_to_bams` as `int32_t` | `MathHelper::bams_to_rad(read_i32)` | C hex of `uint32_t` |
| `BAMS16F` | 2 | `BAMSFHelper::RadToBAMSF` cast to `int16_t` | `BAMSFHelper::BAMSFToRad(read_i16)` | C hex of `uint16_t` |
| `BAMS32F` | 4 | `BAMSFHelper::RadToBAMSF` as `int32_t` | `BAMSFHelper::BAMSFToRad(read_i32)` | C hex of `uint32_t` |

Do not implement `GetReader()` / `GetWriter()` as required API unless desired for local convenience. In C++, direct switch functions are simpler and inline well.

---

# Phase 4 - `Color`

## 4.1 Shape

Implement a compact value type:

```cpp
struct Color {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    constexpr Color() = default;
    constexpr Color(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 0xFF);
    Color(float r, float g, float b, float a = 1.0f);
    explicit Color(Vector4 rgba);
};
```

Static constants:

```cpp
static const Color ColorWhite;
static const Color ColorBlack;
static const Color ColorRed;
static const Color ColorGreen;
static const Color ColorBlue;
static const Color ColorTransparent;
```

## 4.2 Getter/setter equivalents

Replace C# properties with explicit methods:

```cpp
float red_f() const;
void set_red_f(float value);

float green_f() const;
void set_green_f(float value);

float blue_f() const;
void set_blue_f(float value);

float alpha_f() const;
void set_alpha_f(float value);

Vector4 float_vector() const;
void set_float_vector(Vector4 value);

std::uint32_t rgba() const;
void set_rgba(std::uint32_t value);

std::uint32_t argb() const;
void set_argb(std::uint32_t value);

std::uint16_t argb4() const;
void set_argb4(std::uint16_t value);

std::uint16_t rgb565() const;
void set_rgb565(std::uint16_t value);

std::string hex() const;
void set_hex(std::string_view value);
```

Important: port the current C# bit-packing formulas exactly first, even where suspicious. Add tests to lock behavior. If later you decide the C# formulas are bugs, fix intentionally in a separate change.

Known suspicious areas to test carefully:

- `ARGB4` getter masks shifted red/alpha with `0xF`.
- `RGB565` getter masks shifted red with `0x1F` after shifting left.
- `FloatVector` setter in C# assigns `BlueF = value.Y` and `GreenF = value.Z`, which looks swapped. Preserve or explicitly decide to fix. For this behavior-preserving port, preserve first and test.

## 4.3 Methods and operators

Implement:

```cpp
float luminance() const;
static Color lerp(Color from, Color to, float t);
static float distance(Color from, Color to);

Color operator+(Color lhs, Color rhs);
Color operator-(Color lhs, Color rhs);
Color operator*(Color lhs, float rhs);
Color operator*(float lhs, Color rhs);
Color operator/(Color lhs, float rhs);
bool operator==(Color lhs, Color rhs);
bool operator!=(Color lhs, Color rhs);
std::string to_string(Color value); // or rely on Color::hex()
```

Behavior:

- `+` saturates byte components to 255.
- `-` saturates byte components to 0.
- `*` and `/` operate through float channels and clamp to `[0, 1]`.
- Equality is exact component equality.

---

# Phase 5 - Lookup and labeled collection support

`EndianIOExtensions` depends on `BaseLUT`, `LabeledArray<T>`, and `LabeledReadOnlyArray<T>`. Implement enough to preserve the C# behavior while allowing native C++ growth.

## 5.1 `LabeledArray<T>`

```cpp
template<class T>
struct LabeledArray {
    std::string label;
    std::vector<T> values;

    LabeledArray() = default;
    explicit LabeledArray(std::vector<T> values_);
    LabeledArray(std::string label_, std::vector<T> values_);
};
```

## 5.2 `LabeledReadOnlyArray<T>`

C++ has no direct equivalent to C# read-only arrays unless ownership and view are separated. Use one of these:

Preferred simple option:

```cpp
template<class T>
struct LabeledReadOnlyArray {
    std::string label;
    std::shared_ptr<const std::vector<T>> values;
};
```

Alternative if shared ownership is undesirable:

```cpp
template<class T>
struct LabeledReadOnlyArray {
    std::string label;
    std::vector<T> values; // immutable by API convention
    std::span<const T> span() const;
};
```

Use the simpler owning `std::vector<T>` version unless the surrounding code needs shared immutable arrays.

## 5.3 `BaseLUT`

Implement read-side and write-side caches separately.

Write-side default identity behavior:

- Use pointer identity by default.
- Key collections by `const void*` address of the collection object passed to the write helper.
- Null collection maps to address `0`, unless existing SA3D behavior expects a different sentinel.

Optional value-based dedup behavior:

- Add a mode enum:

```cpp
enum class DedupMode {
    PointerIdentity,
    Value
};
```

- For value mode, require either:
  - `std::hash<Range>` / custom hasher, or
  - a caller-provided content key function.

Recommended API shape:

```cpp
class BaseLUT {
public:
    explicit BaseLUT(DedupMode mode = DedupMode::PointerIdentity);

    template<class Range, class Factory>
    std::uint32_t get_add_address(const Range* values, Factory factory);

    template<class Range, class Hash, class Equal, class Factory>
    std::uint32_t get_add_address_by_value(
        const Range& values,
        Hash hash,
        Equal equal,
        Factory factory);

    template<class T, class Factory>
    std::vector<T> get_add_value(std::uint32_t address, Factory factory);

    template<class T, class Factory>
    LabeledArray<T> get_add_labeled_value(
        std::uint32_t address,
        std::string_view generated_prefix,
        Factory factory);

    template<class T, class Factory>
    LabeledReadOnlyArray<T> get_add_labeled_read_only_value(
        std::uint32_t address,
        std::string_view generated_prefix,
        Factory factory);
};
```

Implementation notes:

- `get_add_value` caches by source address.
- Labeled value creation should use an existing label if one is known, otherwise generate something like `<prefix>_<hex address>`.
- Keep label-generation behavior isolated so it can be changed without touching I/O helpers.

---

# Phase 6 - `EndianIOExtensions`

Implement as free functions in a namespace, not as methods on reader/writer.

Recommended namespace:

```cpp
namespace Sa3Dport::Structs::EndianIOExtensions {
    // free functions
}
```

## 6.1 Color I/O

```cpp
void write_color(EndianStackWriter& writer, Color color, ColorIOType type);
Color read_color(const EndianStackReader& reader, std::uint32_t& address, ColorIOType type);
Color read_color(const EndianStackReader& reader, std::uint32_t address, ColorIOType type);
```

Behavior:

- `RGBA8`: write/read `Color::rgba()` as `uint32_t`.
- `ARGB8_32`: write/read `Color::argb()` as `uint32_t`.
- `ARGB8_16`: write/read low 16 bits first, then high 16 bits. This is important for big-endian ARGB behavior.
- `ARGB4`: write/read `Color::argb4()` as `uint16_t`.
- `RGB565`: write/read `Color::rgb565()` as `uint16_t`.
- Address-advancing reader increments by 4 or 2 exactly as the C# version does.

## 6.2 Vector I/O

```cpp
void write_vector2(EndianStackWriter& writer, Vector2 value,
                   FloatIOType type = FloatIOType::Float);
void write_vector3(EndianStackWriter& writer, Vector3 value,
                   FloatIOType type = FloatIOType::Float);

Vector2 read_vector2(const EndianStackReader& reader, std::uint32_t& address,
                     FloatIOType type = FloatIOType::Float);
Vector2 read_vector2(const EndianStackReader& reader, std::uint32_t address,
                     FloatIOType type = FloatIOType::Float);

Vector3 read_vector3(const EndianStackReader& reader, std::uint32_t& address,
                     FloatIOType type = FloatIOType::Float);
Vector3 read_vector3(const EndianStackReader& reader, std::uint32_t address,
                     FloatIOType type = FloatIOType::Float);
```

Behavior:

- Use `write_float_as` / `read_float_as` for each component.
- Advance by `byte_size(type)` per component.

## 6.3 Quaternion I/O

```cpp
void write_quaternion(EndianStackWriter& writer, Quaternion value);
Quaternion read_quaternion(const EndianStackReader& reader, std::uint32_t& address);
Quaternion read_quaternion(const EndianStackReader& reader, std::uint32_t address);
```

Behavior:

- Serialized order is W, X, Y, Z.
- In-memory struct remains X, Y, Z, W.
- Reader advances by 16 bytes.

## 6.4 Collection write helpers

```cpp
template<class Range, class WriteFn>
std::uint32_t write_collection(
    EndianStackWriter& writer,
    const Range& values,
    WriteFn write);

template<class Range, class WriteFn, class PreWriteFn>
std::uint32_t write_collection(
    EndianStackWriter& writer,
    const Range& values,
    WriteFn write,
    PreWriteFn pre_write);
```

Behavior:

1. If `pre_write` is provided, call it for every value before recording `pointer_position()`.
2. Record `pointer_position()`.
3. Write every value with `write(writer, value)`.
4. Return the recorded address.

Use templated callables rather than `std::function` unless type erasure is required.

## 6.5 Collection write helpers with LUT

```cpp
template<class Range, class WriteFn, class PreWriteFn>
std::uint32_t write_collection_with_lut(
    EndianStackWriter& writer,
    const Range* values,
    WriteFn write,
    PreWriteFn pre_write,
    BaseLUT& lut);

template<class Range, class WriteFn>
std::uint32_t write_collection_with_lut(
    EndianStackWriter& writer,
    const Range* values,
    WriteFn write,
    BaseLUT& lut);
```

Default behavior:

- `nullptr` values should return `0` or whatever `BaseLUT::get_add_address(nullptr, ...)` defines.
- Non-null values use pointer identity by default.
- Provide a parallel value-dedup overload if needed:

```cpp
template<class Range, class WriteFn, class Hash, class Equal>
std::uint32_t write_collection_with_value_lut(
    EndianStackWriter& writer,
    const Range& values,
    WriteFn write,
    Hash hash,
    Equal equal,
    BaseLUT& lut);
```

## 6.6 Array read helpers

```cpp
template<class T, class ReadAdvanceFn>
std::vector<T> read_array(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    ReadAdvanceFn read);

template<class T, class ReadFn>
std::vector<T> read_array(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    std::uint32_t element_byte_size,
    ReadFn read);
```

The first form lets `read(reader, address_ref)` advance the address. The second form calls `read(reader, address)` and manually advances by `element_byte_size`.

## 6.7 Array read helpers with LUT

```cpp
template<class T, class ReadAdvanceFn>
std::vector<T> read_array_with_lut(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    ReadAdvanceFn read,
    BaseLUT& lut);

template<class T, class ReadFn>
std::vector<T> read_array_with_lut(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    std::uint32_t element_byte_size,
    ReadFn read,
    BaseLUT& lut);
```

## 6.8 Labeled arrays

```cpp
template<class T, class ReadAdvanceFn>
LabeledArray<T> read_labeled_array(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    ReadAdvanceFn read,
    std::string_view generated_prefix,
    BaseLUT& lut);

template<class T, class ReadFn>
LabeledArray<T> read_labeled_array(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    std::uint32_t element_byte_size,
    ReadFn read,
    std::string_view generated_prefix,
    BaseLUT& lut);
```

Also implement `read_labeled_read_only_array` variants if the source project uses them.

---

# Phase 7 - Utility ports

## 7.1 `VectorUtilities.h`

Implement free functions:

```cpp
float greatest_value(Vector3 vector);
Vector3 calculate_average(std::span<const Vector3> points);
Vector3 calculate_center(std::span<const Vector3> points);
Vector3 normal_to_xz_angles(Vector3 normal);
Vector3 xz_angles_to_normal(Vector3 rotation);
bool is_distance_approximate(Vector3 value, Vector3 other, float epsilon = 0.001f);
bool is_distance_approximate(Vector2 value, Vector2 other, float epsilon = 0.001f);
```

Behavior details:

- `calculate_average` returns zero for an empty span.
- `calculate_center` computes the center of the axis-aligned bounding box of the input points.
- `normal_to_xz_angles` preserves the C# special cases for near +/-Z normals.
- Approximate distance compares squared distance to `epsilon * epsilon`.

## 7.2 `MatrixUtilities.h/.cpp`

Implement:

```cpp
Matrix4x4 create_rotation_matrix(Vector3 rotation, bool zyx);
Matrix4x4 create_transform_matrix(Vector3 position, Quaternion rotation, Vector3 scale);
Matrix4x4 create_transform_matrix(Vector3 position, Vector3 rotation, Vector3 scale, bool rotate_zyx);
Matrix4x4 get_normal_matrix(const Matrix4x4& matrix);
Vector3 to_compatible_euler(const Matrix4x4& matrix, Vector3 previous, bool rotate_zyx);
Vector3 to_euler(const Matrix4x4& matrix, bool rotate_zyx);
```

Implementation requirements:

- Copy the C# rotation matrix formulas exactly.
- For `zyx == true`, preserve the comment/behavior that it is effectively `matZ * matX * matY`.
- For `zyx == false`, preserve `matX * matY * matZ` behavior.
- Transform matrix must be `scale * rotation * translation`.
- Normal matrix must invert and transpose; throw `std::runtime_error` if inversion fails.
- Preserve the two-candidate Euler extraction and compatibility selection algorithm.

Potential source bug to preserve first:

- In `CompatibleEuler`, the C# condition `else if(dif[i] < piThreshold)` likely intended `-piThreshold`, but behavior-preserving port should copy it first and lock with tests. Fix only in a separate intentional behavior-change commit.

## 7.3 `QuaternionUtilities.h/.cpp`

Implement:

```cpp
Vector3 quaternion_to_euler(Quaternion quaternion, bool rotate_zyx);
Vector3 quaternion_to_compatible_euler(Quaternion rotation, Vector3 previous, bool rotate_zyx);
Quaternion euler_to_quaternion(Vector3 rotation, bool rotate_zyx);
Quaternion real_lerp(Quaternion from, Quaternion to, float t);
```

Implementation requirements:

- Normalize the quaternion before converting to Euler.
- Preserve singularity thresholds `0.4995f`.
- Normalize output Euler components into `[-pi, pi]`.
- `quaternion_to_compatible_euler` should normalize quaternion, create a rotation matrix, and call `MatrixUtilities::to_compatible_euler`.
- `euler_to_quaternion` should create a rotation matrix through `MatrixUtilities::create_rotation_matrix` and then call `Quaternion::create_from_rotation_matrix`.
- `real_lerp` should linearly interpolate raw components without normalization or shortest-path correction.

---

# Phase 8 - Struct ports

## 8.1 `Bounds.h/.cpp`

Use a class to preserve C# property behavior.

```cpp
class Bounds {
public:
    Bounds();
    Bounds(Vector3 position, float radius);

    const Vector3& position() const;
    void set_position(Vector3 value);

    float radius() const;
    void set_radius(float value);

    const Matrix4x4& matrix() const;

    static Bounds from_points(std::span<const Vector3> points);

    static Bounds read(const EndianStackReader& reader, std::uint32_t& address);
    static Bounds read(const EndianStackReader& reader, std::uint32_t address);
    void write(EndianStackWriter& writer) const;

    std::string to_string() const;

    friend bool operator==(const Bounds& lhs, const Bounds& rhs);
    friend bool operator!=(const Bounds& lhs, const Bounds& rhs);

private:
    Vector3 position_{};
    float radius_ = 0.0f;
    Matrix4x4 matrix_{};

    void recalculate_matrix();
};
```

Behavior:

- Constructor sets position/radius and initializes `matrix_` to `create_scale(radius) * create_translation(position)`.
- `set_position` updates `position_` and calls `recalculate_matrix`.
- `set_radius` updates `radius_` and calls `recalculate_matrix`.
- `from_points` uses `VectorUtilities::calculate_center`, then radius is the maximum distance from that center.
- `read` reads vector3 position, then float radius, then advances address by 4 after radius.
- `write` writes vector3 position, then float radius.
- Equality compares position and radius only, matching C#; matrix is derived and not part of equality.

## 8.2 `PositionNormal.h`

Implement:

```cpp
struct PositionNormal {
    Vector3 position{};
    Vector3 normal{};

    bool operator==(const PositionNormal&) const = default;
};
```

Add a hash functor if needed:

```cpp
struct PositionNormalHash {
    std::size_t operator()(const PositionNormal& value) const noexcept;
};
```

Behavior:

- Exact component equality.
- Hash should combine exact float bit patterns, not approximate values.

---

# Phase 9 - Testing plan

## 9.1 Unit tests for math primitives

- Vector addition/subtraction/scalar multiply/divide.
- Vector indexing and out-of-range behavior.
- Distance calculations.
- Quaternion normalization.
- Matrix identity, scale, translation, multiplication, transpose, inversion.
- Quaternion-to-matrix and matrix-to-quaternion round-trip for simple rotations.

## 9.2 Unit tests for .NET rounding compatibility

- Positive midpoint values.
- Negative midpoint values.
- Values just below/above midpoint.
- Large values near `int16_t`/`int32_t` range boundaries if serialization uses them.

## 9.3 Unit tests for endian I/O

For both little-endian and big-endian writer/reader:

- `u16`, `i16`, `u32`, `i32`, `float` write/read round-trip.
- Known byte sequences for `0x1234`, `0x12345678`, and representative floats.
- Out-of-range reads throw.
- `pointer_position()` increments exactly by bytes written.

## 9.4 Unit tests for `FloatIOType`

For every enum:

- `byte_size` matches expected size.
- `write_float_as` writes expected byte count.
- `read_float_as` reads representative values.
- `print_float_as` matches expected output style.
- BAMS/BAMSF wrap/cast behavior is tested for `0`, `pi / 2`, `pi`, and `2pi`.

## 9.5 Unit tests for `Color`

- Constants match expected channels.
- Float channels clamp and convert as expected.
- `rgba` and `argb` getter/setter round trips.
- `argb4` and `rgb565` preserve current C# behavior.
- Hex parsing supports:
  - `RGB`
  - `RGBA`
  - `RRGGBB`
  - `RRGGBBAA`
  - with and without `#`
  - with spaces removed
- Invalid hex throws.
- Arithmetic operators saturate/clamp as expected.
- `luminance`, `lerp`, and `distance` match C# outputs.

## 9.6 Unit tests for `EndianIOExtensions`

- Write/read color for each `ColorIOType`.
- `ARGB8_16` low-half/high-half ordering.
- Write/read vector2/vector3 for every `FloatIOType`.
- Write/read quaternion uses WXYZ serialized order.
- `read_array` address-advance form.
- `read_array` fixed-element-size form.
- LUT read returns the cached value for repeated address reads.
- LUT write returns the cached address for repeated pointer-identity collection writes.
- Value-dedup overload returns the same address for equal content when explicitly requested.

## 9.7 Unit tests for utilities and structs

- `VectorUtilities::calculate_average` empty/non-empty.
- `VectorUtilities::calculate_center` empty/single/multiple points.
- Normal-to-XZ and XZ-to-normal representative vectors.
- Matrix rotation formulas compared against known C# outputs.
- `to_euler` and `to_compatible_euler` compared against known C# outputs.
- Quaternion utility conversions compared against known C# outputs.
- `Bounds` matrix recalculates after `set_position` and `set_radius`.
- `Bounds::read` advances by 16 bytes total: 12 for position and 4 for radius.
- `PositionNormal` exact equality/hash.

---

# Phase 10 - Implementation order

1. Add `MathCompat.h` and test .NET rounding behavior.
2. Add `Vector2`, `Vector3`, `Vector4` and vector tests.
3. Add `Matrix4x4` core operations and tests.
4. Add `Quaternion` core operations and tests.
5. Add `MathHelper.h`; adapt/extend `BAMSFHelper.h` wrappers.
6. Add `Endian.h`, `EndianStackReader`, `EndianStackWriter` and endian tests.
7. Add `ColorIOType.h` and `FloatIOType.h` with tests.
8. Add `Color.h` with packing/hex/arithmetic tests.
9. Add `BaseLUT`, `LabeledArray`, `LabeledReadOnlyArray`.
10. Add `EndianIOExtensions.h` and I/O extension tests.
11. Add `VectorUtilities.h` and tests.
12. Add `MatrixUtilities.h/.cpp` and tests using known C# output vectors/matrices.
13. Add `QuaternionUtilities.h/.cpp` and tests using known C# output quaternions/eulers.
14. Add `Bounds.h/.cpp` and tests.
15. Add `PositionNormal.h` and hash tests if needed.
16. Run integration tests that serialize in C++ and deserialize in C#, and vice versa, if the C# project can be run as a comparison oracle.

---

# Open questions / future decisions

These do not block the first behavior-preserving port, but should be revisited later.

1. Should suspected C# packing issues in `Color.ARGB4`, `Color.RGB565`, and `Color.FloatVector` be fixed after compatibility tests are established?
2. Should `BaseLUT` value deduplication be generic and automatic, or only opt-in through explicit content key functions?
3. Should C#-style PascalCase aliases be provided temporarily to simplify porting existing call sites?
4. Should `LabeledReadOnlyArray<T>` use shared immutable storage or simple owning vectors?
5. Should matrix inversion be implemented manually or copied from a known permissive implementation? If copied, preserve license attribution.

---

# Source reference map

This document was derived from the following source behavior:

- `EndianIOExtensions.cs`: color/vector/quaternion I/O, array and collection helpers, LUT-backed read/write, labeled arrays.
- `FloatIOType.cs`: byte sizes, printer behavior, BAMS/BAMSF read/write behavior, and `.NET` rounding call sites.
- `ColorIOType.cs`: color format enum and byte sizes.
- `Color.cs`: color channel storage, packed conversions, hex parsing, arithmetic, equality, and string behavior.
- `MatrixUtilities.cs`: rotation matrix formulas, transform composition order, normal matrix, Euler extraction and compatibility selection.
- `QuaternionUtilities.cs`: quaternion/Euler conversion, singularity handling, Euler normalization, compatible Euler conversion, raw component lerp.
- `VectorUtilities.cs`: vector aggregation, center calculation, normal/euler helpers, approximate distance.
- `Bounds.cs`: cached matrix behavior, bounds from points, endian read/write, equality.
- `PositionNormal.cs`: exact position+normal value semantics.
- `BAMSFHelper.h`: existing C++ BAMSF conversion helper and namespace anchor.
