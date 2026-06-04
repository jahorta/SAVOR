#include "GvmTextureModel.h"

namespace soasim::gvm::model {

std::string to_string(const TextureFormat format) {
    switch (format) {
    case TextureFormat::I4: return "I4";
    case TextureFormat::I8: return "I8";
    case TextureFormat::IA4: return "IA4";
    case TextureFormat::IA8: return "IA8";
    case TextureFormat::RGB565: return "RGB565";
    case TextureFormat::RGB5A3: return "RGB5A3";
    case TextureFormat::RGBA8: return "RGBA8";
    case TextureFormat::CI4: return "CI4";
    case TextureFormat::CI8: return "CI8";
    case TextureFormat::CI14X2: return "CI14X2";
    case TextureFormat::CMPR: return "CMPR";
    case TextureFormat::Unknown: return "Unknown";
    default: return "Unknown";
    }
}

std::string to_string(const PaletteFormat format) {
    switch (format) {
    case PaletteFormat::IA8: return "IA8";
    case PaletteFormat::RGB565: return "RGB565";
    case PaletteFormat::RGB5A3: return "RGB5A3";
    case PaletteFormat::None: return "None";
    default: return "Unknown";
    }
}

} // namespace soasim::gvm::model
