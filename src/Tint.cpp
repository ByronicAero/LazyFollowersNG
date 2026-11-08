// Tint - saves the face tint of a loaded actor as a .dds next to the RaceMenu preset
// (Data/Textures/CharGen/Exported/<name>.dds), like "skee preset-save" did. RaceMenu's console command
// exported the player's tint for every target; we read the target's own tint: the FaceGen material of its
// Face head part holds it as a GPU texture, which is copied to a CPU-readable texture and written out
// (top mip only). Runs on the main thread, where the game itself uses the device context.

#include "Tint.h"

#include <d3d11.h>

namespace Tint
{
    namespace
    {
        // Releases a COM pointer when it goes out of scope.
        template <class T>
        struct ComPtr
        {
            T* p = nullptr;
            ~ComPtr()
            {
                if (p) {
                    p->Release();
                }
            }
            T** operator&() { return &p; }
            T*  operator->() const { return p; }
            explicit operator bool() const { return p != nullptr; }
        };

#pragma pack(push, 1)
        struct PixelFormat
        {
            std::uint32_t size = 32;
            std::uint32_t flags = 0;
            std::uint32_t fourCC = 0;
            std::uint32_t rgbBits = 0;
            std::uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
        };
        struct Header
        {
            std::uint32_t size = 124;
            std::uint32_t flags = 0;
            std::uint32_t height = 0;
            std::uint32_t width = 0;
            std::uint32_t pitchOrLinearSize = 0;
            std::uint32_t depth = 0;
            std::uint32_t mipCount = 1;
            std::uint32_t reserved1[11]{};
            PixelFormat   pf;
            std::uint32_t caps = 0x1000;  // DDSCAPS_TEXTURE
            std::uint32_t caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
        };
        struct HeaderDX10
        {
            std::uint32_t format = 0;
            std::uint32_t dimension = 3;  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            std::uint32_t miscFlag = 0;
            std::uint32_t arraySize = 1;
            std::uint32_t miscFlags2 = 0;
        };
#pragma pack(pop)
        static_assert(sizeof(Header) == 124 && sizeof(PixelFormat) == 32 && sizeof(HeaderDX10) == 20);

        constexpr std::uint32_t FourCC(char a, char b, char c, char d)
        {
            return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) | (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24);
        }

        // Block size in bytes for block-compressed formats, 0 if not compressed.
        std::uint32_t BlockBytes(DXGI_FORMAT a_f)
        {
            switch (a_f) {
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
                return 8;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return 16;
            default:
                return 0;
            }
        }

        // Bytes per pixel for the uncompressed formats a tint can have, 0 = not supported.
        std::uint32_t PixelBytes(DXGI_FORMAT a_f)
        {
            switch (a_f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
                return 8;
            default:
                return 0;
            }
        }

        // Fills the header; formats old tools know get the classic header (like RaceMenu's files),
        // the rest a DX10 extension. False = format not supported.
        bool MakeHeader(DXGI_FORMAT a_f, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_rowBytes, std::uint32_t a_rows, Header& a_hdr, bool& a_dx10)
        {
            a_hdr.width = a_w;
            a_hdr.height = a_h;
            a_hdr.flags = 0x1 | 0x2 | 0x4 | 0x1000;  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
            a_dx10 = false;
            if (BlockBytes(a_f)) {
                a_hdr.flags |= 0x80000;  // LINEARSIZE
                a_hdr.pitchOrLinearSize = a_rowBytes * a_rows;
            } else if (PixelBytes(a_f)) {
                a_hdr.flags |= 0x8;  // PITCH
                a_hdr.pitchOrLinearSize = a_rowBytes;
            } else {
                return false;
            }
            auto& pf = a_hdr.pf;
            switch (a_f) {
            case DXGI_FORMAT_BC1_UNORM:
                pf.flags = 0x4;
                pf.fourCC = FourCC('D', 'X', 'T', '1');
                break;
            case DXGI_FORMAT_BC2_UNORM:
                pf.flags = 0x4;
                pf.fourCC = FourCC('D', 'X', 'T', '3');
                break;
            case DXGI_FORMAT_BC3_UNORM:
                pf.flags = 0x4;
                pf.fourCC = FourCC('D', 'X', 'T', '5');
                break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                pf.flags = 0x40 | 0x1;  // RGB | ALPHAPIXELS
                pf.rgbBits = 32;
                pf.rMask = 0x000000FF;
                pf.gMask = 0x0000FF00;
                pf.bMask = 0x00FF0000;
                pf.aMask = 0xFF000000;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                pf.flags = 0x40 | 0x1;
                pf.rgbBits = 32;
                pf.rMask = 0x00FF0000;
                pf.gMask = 0x0000FF00;
                pf.bMask = 0x000000FF;
                pf.aMask = 0xFF000000;
                break;
            default:
                pf.flags = 0x4;
                pf.fourCC = FourCC('D', 'X', '1', '0');
                a_dx10 = true;
                break;
            }
            return true;
        }

        // The FaceGen tint texture of the actor's loaded face, or nullptr (with a reason).
        ID3D11Texture2D* FindTint(RE::Actor* a_actor, std::string& a_error)
        {
            auto obj = a_actor->GetHeadPartObject(RE::BGSHeadPart::HeadPartType::kFace);
            auto geo = obj ? obj->AsGeometry() : nullptr;
            if (!geo) {
                a_error = "no loaded face";
                return nullptr;
            }
            auto prop = geo->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get();
            auto shader = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
            auto mat = shader ? shader->material : nullptr;
            if (!mat || mat->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen) {
                a_error = "the face has no FaceGen tint";
                return nullptr;
            }
            auto tint = static_cast<RE::BSLightingShaderMaterialFacegen*>(mat)->tintTexture.get();
            // NiSourceTexture's renderer texture starts with its ID3D11Texture2D (NiTexture::RendererData)
            auto data = tint ? reinterpret_cast<RE::NiTexture::RendererData*>(tint->rendererTexture) : nullptr;
            if (!data || !data->texture) {
                a_error = "the tint texture is not loaded";
                return nullptr;
            }
            return data->texture;
        }
    }

    bool ExportFaceTint(RE::Actor* a_actor, const std::filesystem::path& a_file, std::string& a_error)
    {
        if (!a_actor) {
            a_error = "no actor";
            return false;
        }
        auto src = FindTint(a_actor, a_error);
        if (!src) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        ComPtr<ID3D11Device> device;
        src->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> ctx;
        if (device) {
            device->GetImmediateContext(&ctx);
        }
        if (!device || !ctx || desc.SampleDesc.Count != 1) {
            a_error = "cannot read the tint texture";
            return false;
        }

        // copy the top mip to a CPU-readable texture
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
            a_error = std::format("could not create a copy texture (format {})", static_cast<int>(desc.Format));
            return false;
        }
        ctx->CopySubresourceRegion(staging.p, 0, 0, 0, 0, src, 0, nullptr);

        const std::uint32_t block = BlockBytes(desc.Format);
        const std::uint32_t pixel = PixelBytes(desc.Format);
        const std::uint32_t rows = block ? (desc.Height + 3) / 4 : desc.Height;
        const std::uint32_t rowBytes = block ? ((desc.Width + 3) / 4) * block : desc.Width * pixel;
        Header              hdr;
        bool                dx10 = false;
        if (!MakeHeader(desc.Format, desc.Width, desc.Height, rowBytes, rows, hdr, dx10)) {
            a_error = std::format("tint format {} is not supported", static_cast<int>(desc.Format));
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.p, 0, D3D11_MAP_READ, 0, &map)) || !map.pData) {
            a_error = "could not read the copy texture";
            return false;
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * rows);
        for (std::uint32_t r = 0; r < rows; ++r) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(r) * rowBytes, static_cast<const std::uint8_t*>(map.pData) + static_cast<std::size_t>(r) * map.RowPitch, rowBytes);
        }
        ctx->Unmap(staging.p, 0);

        std::error_code ec;
        std::filesystem::create_directories(a_file.parent_path(), ec);
        std::ofstream out(a_file, std::ios::binary | std::ios::trunc);
        if (!out) {
            a_error = "could not write " + a_file.string();
            return false;
        }
        out.write("DDS ", 4);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        if (dx10) {
            HeaderDX10 ext;
            ext.format = static_cast<std::uint32_t>(desc.Format);
            out.write(reinterpret_cast<const char*>(&ext), sizeof(ext));
        }
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!out) {
            a_error = "could not write " + a_file.string();
            return false;
        }
        logger::info("Tint {}x{} format {} written to {}", desc.Width, desc.Height, static_cast<int>(desc.Format), a_file.string());
        return true;
    }
}
