﻿﻿﻿#pragma once

#include "RenderAPI.h"
#include "Vec/Vec.hpp"
#include "Mat/Mat4.hpp"
#include <cmath>

/**
 * @brief Orbit Camera
 *
 * 3D camera that orbits around a target point, with pan, zoom, and rotate capabilities
 */
class RENDER_API Camera3D
{
public:
    Camera3D();

    // ==================== Camera Properties ====================

    void setTarget(const Ut::Vec3f& target);
    Ut::Vec3f getTarget() const;

    void setDistance(float distance);
    float getDistance() const;

    /// Azimuth angle (horizontal rotation, radians)
    void setAzimuth(float azimuth);
    float getAzimuth() const;

    /// Elevation angle (vertical rotation, radians), clamped to [-PI/2+epsilon, PI/2-epsilon]
    void setElevation(float elevation);
    float getElevation() const;

    /// Set field of view angle (degrees)
    void setFov(float fovDegrees);

    /// Get field of view angle (degrees)
    float getFov() const;

    /// Set near/far plane
    void setNearPlane(float nearPlane);
    void setFarPlane(float farPlane);

    // ==================== Matrix Computation ====================

    /// Get view matrix (column-major, 16 floats, compatible with OpenGL)
    Ut::Mat4f getViewMatrix() const;

    /// Get projection matrix
    Ut::Mat4f getProjectionMatrix(float aspectRatio) const;

    /// Get camera position
    Ut::Vec3f getPosition() const;

    /// Get forward/right/up direction vectors
    Ut::Vec3f getForward() const;
    Ut::Vec3f getRight() const;
    Ut::Vec3f getUp() const;

    // ==================== Interaction Operations ====================

    /// Rotate (deltaX, deltaY are mouse drag pixels)
    void rotate(float deltaX, float deltaY);

    /// Pan (screen space translation amount)
    void pan(float deltaX, float deltaY);

    /// Zoom (delta is scroll wheel increment, positive for zoom in)
    void zoom(float delta);

    /// Reset to default view
    void reset();

    /// Focus on bounding box
    void focusOnBBox(const Ut::Vec3f& bboxMin, const Ut::Vec3f& bboxMax);

    // ==================== Parameters ====================

    float m_rotateSensitivity{ 0.005f };
    float m_panSensitivity{ 1.0f };
    float m_zoomSensitivity{ 0.1f };
    float m_minDistance{ 0.1f };
    float m_maxDistance{ 1000.0f };

    // ==================== Preset Views ====================

    /// Preset view type
    enum class ViewPreset
    {
        Front,      ///< Front view
        Back,       ///< Back view
        Left,       ///< Left view
        Right,      ///< Right view
        Top,        ///< Top view
        Bottom,     ///< Bottom view
        Home,       ///< Default view

        Isometric,  ///< Isometric view (45 deg azimuth, 35.26 deg elevation)
        Dimetric,   ///< Dimetric view
        Trimetric,  ///< Trimetric view
        SE_Isometric, ///< Southeast isometric view
        SW_Isometric, ///< Southwest isometric view
        NE_Isometric, ///< Northeast isometric view
        NW_Isometric  ///< Northwest isometric view
    };

    /// Set to a preset view
    void setViewPreset(ViewPreset preset);

private:
    void clampElevation();

private:
    Ut::Vec3f m_target;
    float m_distance;
    float m_azimuth;      // Horizontal angle (radians)
    float m_elevation;    // Elevation angle (radians)
    float m_fov;          // Field of view (degrees)
    float m_nearPlane;
    float m_farPlane;
};