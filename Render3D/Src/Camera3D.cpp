#include "Render3D/Camera3D.h"
#include <algorithm>

Camera3D::Camera3D()
    : m_target(0.0f, 0.0f, 0.0f)
    , m_distance(10.0f)
    , m_azimuth(0.0f)
    , m_elevation(0.5f)
    , m_fov(45.0f)
    , m_nearPlane(0.1f)
    , m_farPlane(1000.0f)
{
}

void Camera3D::setTarget(const Ut::Vec3f& target)
{
    m_target = target;
}
Ut::Vec3f Camera3D::getTarget() const
{
    return m_target;
}

void Camera3D::setDistance(float distance)
{
    m_distance = std::clamp(distance, m_minDistance, m_maxDistance);
}
float Camera3D::getDistance() const
{
    return m_distance;
}

void Camera3D::setAzimuth(float azimuth)
{
    m_azimuth = azimuth;
}
float Camera3D::getAzimuth() const
{
    return m_azimuth;
}

void Camera3D::setElevation(float elevation)
{
    m_elevation = elevation;
    clampElevation();
}
float Camera3D::getElevation() const
{
    return m_elevation;
}

void Camera3D::setFov(float fovDegrees)
{
    m_fov = fovDegrees;
}
float Camera3D::getFov() const
{
    return m_fov;
}
void Camera3D::setNearPlane(float np)
{
    m_nearPlane = np;
}
void Camera3D::setFarPlane(float fp)
{
    m_farPlane = fp;
}

Ut::Vec3f Camera3D::getPosition() const
{
    float cosEl = std::cos(m_elevation);
    float sinEl = std::sin(m_elevation);
    float cosAz = std::cos(m_azimuth);
    float sinAz = std::sin(m_azimuth);

    return Ut::Vec3f(
        m_target[0] + m_distance * cosEl * sinAz,
        m_target[1] + m_distance * sinEl,
        m_target[2] + m_distance * cosEl * cosAz
    );
}

Ut::Vec3f Camera3D::getForward() const
{
    return (m_target - getPosition()).normalized();
}

Ut::Vec3f Camera3D::getRight() const
{
    Ut::Vec3f up(0.0f, 1.0f, 0.0f);
    return getForward().cross(up).normalized();
}

Ut::Vec3f Camera3D::getUp() const
{
    return getRight().cross(getForward()).normalized();
}

Ut::Mat4f Camera3D::getViewMatrix() const
{
    return Ut::Mat4f::lookAt(getPosition(), m_target, Ut::Vec3f(0.0f, 1.0f, 0.0f));
}

Ut::Mat4f Camera3D::getProjectionMatrix(float aspectRatio) const
{
    float fovRad = m_fov * 3.14159265358979323846f / 180.0f;
    return Ut::Mat4f::perspective(fovRad, aspectRatio, m_nearPlane, m_farPlane);
}

void Camera3D::rotate(float deltaX, float deltaY)
{
    m_azimuth += deltaX * m_rotateSensitivity;
    m_elevation += deltaY * m_rotateSensitivity;
    clampElevation();
}

void Camera3D::pan(float deltaX, float deltaY)
{
    Ut::Vec3f right = getRight();
    Ut::Vec3f up = getUp();
    float factor = m_distance * m_panSensitivity * 0.001f;
    m_target = m_target - right * (deltaX * factor) + up * (deltaY * factor);
}

void Camera3D::zoom(float delta)
{
    m_distance -= delta * m_distance * m_zoomSensitivity;
    m_distance = std::clamp(m_distance, m_minDistance, m_maxDistance);
}

void Camera3D::reset()
{
    m_target = Ut::Vec3f(0.0f, 0.0f, 0.0f);
    m_distance = 10.0f;
    m_azimuth = 0.0f;
    m_elevation = 0.5f;
}

void Camera3D::focusOnBBox(const Ut::Vec3f& bboxMin, const Ut::Vec3f& bboxMax)
{
    m_target = (bboxMin + bboxMax) * 0.5f;
    Ut::Vec3f size = bboxMax - bboxMin;
    float diag = std::sqrt(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]);
    m_distance = diag * 2.0f;
    if (m_distance < m_minDistance) m_distance = m_minDistance;
    if (m_distance > m_maxDistance) m_distance = m_maxDistance;
}

void Camera3D::clampElevation()
{
    constexpr float epsilon = 0.01f;
    constexpr float halfPi = 3.14159265358979323846f / 2.0f;
    m_elevation = std::clamp(m_elevation, -halfPi + epsilon, halfPi - epsilon);
}

void Camera3D::setViewPreset(ViewPreset preset)
{
    constexpr float halfPi = 3.14159265358979323846f / 2.0f;
    constexpr float pi = 3.14159265358979323846f;
    constexpr float epsilon = 0.01f;

    switch (preset)
    {
        case ViewPreset::Front:
            m_azimuth = 0.0f;
            m_elevation = 0.0f;
            break;
        case ViewPreset::Back:
            m_azimuth = pi;
            m_elevation = 0.0f;
            break;
        case ViewPreset::Left:
            m_azimuth = -halfPi;
            m_elevation = 0.0f;
            break;
        case ViewPreset::Right:
            m_azimuth = halfPi;
            m_elevation = 0.0f;
            break;
        case ViewPreset::Top:
            m_azimuth = 0.0f;
            m_elevation = halfPi - epsilon;
            break;
        case ViewPreset::Bottom:
            m_azimuth = 0.0f;
            m_elevation = -halfPi + epsilon;
            break;
        case ViewPreset::Home:
            reset();
            break;
        case ViewPreset::Isometric:
            m_azimuth = -halfPi * 0.5f;
            m_elevation = std::atan(1.0f / std::sqrt(2.0f));
            break;
        case ViewPreset::Dimetric:
            m_azimuth = -halfPi * 0.5f;
            m_elevation = halfPi * 0.33f;
            break;
        case ViewPreset::Trimetric:
            m_azimuth = -halfPi * 0.444f;
            m_elevation = halfPi * 0.222f;
            break;
        case ViewPreset::SE_Isometric:
            m_azimuth = halfPi * 0.5f;
            m_elevation = std::atan(1.0f / std::sqrt(2.0f));
            break;
        case ViewPreset::SW_Isometric:
            m_azimuth = -halfPi * 0.5f;
            m_elevation = -std::atan(1.0f / std::sqrt(2.0f));
            break;
        case ViewPreset::NE_Isometric:
            m_azimuth = halfPi * 0.5f;
            m_elevation = -std::atan(1.0f / std::sqrt(2.0f));
            break;
        case ViewPreset::NW_Isometric:
            m_azimuth = -halfPi * 0.5f;
            m_elevation = std::atan(1.0f / std::sqrt(2.0f));
            break;
    }
}