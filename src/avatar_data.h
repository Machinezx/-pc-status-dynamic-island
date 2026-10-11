#pragma once

// 停靠细条里的角色头像位图（"大肥鱼"）。
//
// 素材来自 dsh-pet 的 assets\big_blue_fat_fish\，由 tools\make_avatar.py 在构建前
// 裁剪（按 alpha 边框）并缩放成 ≤64px 的原始 RGBA8 数组，直接编进 exe——运行时
// 不读任何外部文件、不依赖 GDI+ 或任何图片解码器。
namespace island_asset {

struct Image {
    int                w = 0;
    int                h = 0;
    const unsigned char* rgba = nullptr;   // RGBA8，直通 alpha，自上而下，w*h*4 字节
};

const Image& AvatarImage();

}  // namespace island_asset
