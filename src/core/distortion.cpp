#include "distortion.h"

#include <cmath>

namespace psvr2pt {

using namespace DirectX;

// Rational radial + Brown tangential + rotation distortion model.
// Inputs (x, y) are focal-length-normalised image coordinates: (u-cx)/fx, (v-cy)/fy.
// Output is the distorted normalised coordinate (same convention).
XMFLOAT2 get_distorted_point(double x, double y, const CameraParameters& params) {
    const auto& p = params.coeffs;

    const double xSq   = x * x;
    const double ySq   = y * y;
    const double rSq   = xSq + ySq;
    const double twoXY = 2.0 * x * y;
    const double rSqP4 = rSq * rSq * rSq * rSq;  // r^8
    const double rSqP5 = rSqP4 * rSq;             // r^10

    // Rational radial numerator: 1 + p[0]*r² + p[1]*r⁴ + p[4]*r⁶ + p[14]*r⁸ + p[15]*r¹⁰ + p[16]*r¹²
    const double numPoly = 1.0
        + (((p[4] * rSq + p[1]) * rSq + p[0]) * rSq)
        + (p[16] * rSqP5 * rSq)
        + (p[15] * rSqP5)
        + (p[14] * rSqP4);

    // Rational radial denominator: 1 + p[5]*r² + p[6]*r⁴ + p[7]*r⁶ + p[17]*r⁸ + p[18]*r¹⁰ + p[19]*r¹²
    const double denPoly = 1.0
        + (((p[7] * rSq + p[6]) * rSq + p[5]) * rSq)
        + (p[19] * rSqP5 * rSq)
        + (p[18] * rSqP5)
        + (p[17] * rSqP4);

    const double radialScale = (std::abs(denPoly) > 1e-9) ? (numPoly / denPoly) : 1.0;

    // Brown tangential + secondary radial correction.
    // p[2]=p2, p[3]=p1  (note: order is swapped vs OpenCV convention)
    const double distortedXTerm = (((p[9] * rSq + p[8]) * rSq))
                                + ((xSq + xSq + rSq) * p[3])
                                + (radialScale * x)
                                + (twoXY * p[2]);

    const double distortedYTerm = (((p[11] * rSq + p[10]) * rSq))
                                + (twoXY * p[3])
                                + ((ySq + ySq + rSq) * p[2])
                                + (radialScale * y);

    // Small-angle rotation correction (p[12] = X-axis angle, p[13] = Y-axis angle, radians).
    XMMATRIX Rx = XMMatrixRotationAxis(XMVectorSet(1.f, 0.f, 0.f, 0.f), static_cast<float>(p[12]));
    XMMATRIX Ry = XMMatrixRotationAxis(XMVectorSet(0.f, 1.f, 0.f, 0.f), static_cast<float>(p[13]));
    XMMATRIX R  = XMMatrixMultiply(Rx, Ry);

    XMVECTOR pIn  = XMVectorSet(static_cast<float>(distortedXTerm),
                                static_cast<float>(distortedYTerm),
                                1.0f, 0.0f);
    XMVECTOR pOut = XMVector3Transform(pIn, R);
    const float w = XMVectorGetZ(pOut);

    if (std::abs(w) < 1e-9f) {
        return { -10.0f, -10.0f };  // outside valid sample range
    }

    XMFLOAT2 result;
    result.x = XMVectorGetX(pOut) / w;
    result.y = XMVectorGetY(pOut) / w;
    return result;
}

void create_undistortion_mesh(int textureWidth, int textureHeight,
                              float zoomFactor,
                              float signed_toe_out_rad,
                              float tilt_down_rad,
                              float signed_roll_rad,
                              const CameraIntrinsics& intr,
                              const CameraParameters& params,
                              std::vector<UndistortVertex>& outVerts,
                              std::vector<unsigned long>& outIdx,
                              unsigned long densityX,
                              unsigned long densityY) {
    outVerts.clear();
    outIdx.clear();
    outVerts.reserve((densityX + 1) * (densityY + 1));
    outIdx.reserve(densityX * densityY * 6);

    // Precompute mounting rotation: R = Rz(signed_roll) * Rx(tilt_down) * Ry(signed_toe_out)
    // Transforms a ray from "desired forward output space" into camera space.
    // Camera image convention: +x = right, +y = DOWN, +z = into scene.
    // signed_roll > 0 rotates the image CW (left eye); < 0 rotates CCW (right eye).
    const double ct = std::cos(static_cast<double>(signed_toe_out_rad));
    const double st = std::sin(static_cast<double>(signed_toe_out_rad));
    const double cp = std::cos(static_cast<double>(tilt_down_rad));
    const double sp = std::sin(static_cast<double>(tilt_down_rad));
    const double cr = std::cos(static_cast<double>(signed_roll_rad));
    const double sr = std::sin(static_cast<double>(signed_roll_rad));

    for (unsigned long j = 0; j <= densityY; ++j) {
        for (unsigned long i = 0; i <= densityX; ++i) {
            const float uOut = static_cast<float>(i) / densityX;
            const float vOut = static_cast<float>(j) / densityY;

            const float pxOut = uOut * textureWidth;
            const float pyOut = vOut * textureHeight;

            UndistortVertex v;
            v.Pos = XMFLOAT3{ uOut * 2.0f - 1.0f, (1.0f - vOut) * 2.0f - 1.0f, 0.0f };

            // Ray in desired output space (zoom-scaled, centred on headset-forward).
            double rx = ((pxOut - intr.cx) / intr.fx) * zoomFactor;
            double ry = ((pyOut - intr.cy) / intr.fy) * zoomFactor;
            double rz = 1.0;

            // Apply Ry(signed_toe_out): yaw the ray to account for horizontal mounting.
            const double rx1 =  ct * rx + st * rz;
            const double ry1 =  ry;
            const double rz1 = -st * rx + ct * rz;

            // Apply Rx(tilt_down): pitch the ray to account for downward tilt.
            const double rx2 = rx1;
            const double ry2 = cp * ry1 - sp * rz1;
            const double rz2 = sp * ry1 + cp * rz1;

            // Apply Rz(signed_roll): rotate around optical axis for camera roll.
            // +roll = CW in image (y-down convention); left eye +, right eye -.
            const double rx3 = cr * rx2 - sr * ry2;
            const double ry3 = sr * rx2 + cr * ry2;
            const double rz3 = rz2;

            // Project back to normalised camera coords for distortion lookup.
            const double xLookup = (rz3 > 1e-9) ? rx3 / rz3 : 0.0;
            const double yLookup = (rz3 > 1e-9) ? ry3 / rz3 : 0.0;

            const XMFLOAT2 d = get_distorted_point(xLookup, yLookup, params);

            v.Tex = XMFLOAT2{
                static_cast<float>(d.x * intr.fx + intr.cx) / textureWidth,
                static_cast<float>(d.y * intr.fy + intr.cy) / textureHeight
            };
            outVerts.push_back(v);
        }
    }

    const unsigned long stride = densityX + 1;
    for (unsigned long j = 0; j < densityY; ++j) {
        for (unsigned long i = 0; i < densityX; ++i) {
            const unsigned long tl = j * stride + i;
            const unsigned long tr = tl + 1;
            const unsigned long bl = (j + 1) * stride + i;
            const unsigned long br = bl + 1;
            outIdx.insert(outIdx.end(), { tl, tr, bl, bl, tr, br });
        }
    }
}

void create_default_mesh(int /*imageWidth*/, int /*imageHeight*/,
                         int /*textureWidth*/, int /*textureHeight*/,
                         std::vector<UndistortVertex>& outVerts,
                         std::vector<unsigned long>& outIdx) {
    outVerts = {
        { { -1.f,  1.f, 0.f }, { 0.f, 0.f } },
        { {  1.f,  1.f, 0.f }, { 1.f, 0.f } },
        { { -1.f, -1.f, 0.f }, { 0.f, 1.f } },
        { {  1.f, -1.f, 0.f }, { 1.f, 1.f } },
    };
    outIdx = { 0, 1, 2, 2, 1, 3 };
}

}  // namespace psvr2pt
