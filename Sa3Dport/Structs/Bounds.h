#pragma once

#include "Structs/EndianIOExtensions.h"
#include "Structs/Matrix4x4.h"
#include "Structs/VectorUtilities.h"

#include <algorithm>
#include <locale>
#include <sstream>
#include <span>
#include <string>

namespace Sa3Dport::Structs {

class Bounds {
public:
    Bounds() {
        recalculate_matrix();
    }

    Bounds(Vector3 position, float radius) : position_(position), radius_(radius) {
        recalculate_matrix();
    }

    [[nodiscard]] const Vector3& position() const { return position_; }

    void set_position(Vector3 value) {
        position_ = value;
        recalculate_matrix();
    }

    [[nodiscard]] float radius() const { return radius_; }

    void set_radius(float value) {
        radius_ = value;
        recalculate_matrix();
    }

    [[nodiscard]] const Matrix4x4& matrix() const { return matrix_; }

    [[nodiscard]] static Bounds from_points(std::span<const Vector3> points) {
        const Vector3 center = VectorUtilities::calculate_center(points);
        float radius = 0.0f;
        for (const auto& point : points) {
            radius = std::max(radius, distance(center, point));
        }
        return {center, radius};
    }

    [[nodiscard]] static Bounds read(const EndianStackReader& reader, std::size_t address) {
        std::uint32_t cursor = static_cast<std::uint32_t>(address);
        return read(reader, cursor);
    }

    [[nodiscard]] static Bounds read(const EndianStackReader& reader, std::uint32_t& address) {
        const Vector3 position = EndianIOExtensions::read_vector3(reader, address);
        const float radius = reader.read_float(address);
        address += 4;
        return {position, radius};
    }

    void write(EndianStackWriter& writer) const {
        EndianIOExtensions::write_vector3(writer, position_);
        writer.write_float(radius_);
    }

    [[nodiscard]] std::string to_string() const {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "Position: (" << position_.x << ", " << position_.y << ", " << position_.z
               << "), Radius: " << radius_;
        return stream.str();
    }

    [[nodiscard]] friend bool operator==(const Bounds& lhs, const Bounds& rhs) {
        return lhs.position_ == rhs.position_ && lhs.radius_ == rhs.radius_;
    }

    [[nodiscard]] friend bool operator!=(const Bounds& lhs, const Bounds& rhs) {
        return !(lhs == rhs);
    }

private:
    void recalculate_matrix() {
        matrix_ = create_scale(radius_) * create_translation(position_);
    }

    Vector3 position_ {};
    float radius_ = 0.0f;
    Matrix4x4 matrix_ {};
};

} // namespace Sa3Dport::Structs
