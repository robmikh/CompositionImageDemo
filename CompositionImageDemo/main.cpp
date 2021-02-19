#include "pch.h"
#include "MainWindow.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::Graphics::Imaging;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Streams;
    using namespace Windows::UI;
    using namespace Windows::UI::Composition;
}

namespace util
{
    using namespace robmikh::common::uwp;
    using namespace robmikh::common::desktop;
}

// We can only use IAsync* with WinRT objects
std::future<winrt::com_ptr<ID3D11Texture2D>> CreateTextureFromImageAsync(winrt::com_ptr<ID3D11Device> const& d3dDevice)
{
    // You'll need to get a stream to your file. This demo uses a local image.
    auto currentPath = std::filesystem::current_path();
    auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(currentPath.wstring());
    auto file = co_await folder.GetFileAsync(L"tripphoto1.jpg");

    winrt::com_ptr<ID3D11Texture2D> texture;
    {
        auto stream = co_await file.OpenReadAsync();

        // Create the decoder for our image
        auto decoder = co_await winrt::BitmapDecoder::CreateAsync(stream);
        // Since this image is a jpg it only has a single frame
        auto frame = co_await decoder.GetFrameAsync(0);
        auto width = frame.PixelWidth();
        auto height = frame.PixelHeight();
        WINRT_ASSERT(frame.BitmapPixelFormat() == winrt::BitmapPixelFormat::Bgra8);

        auto pixelData = co_await frame.GetPixelDataAsync();
        auto bytes = pixelData.DetachPixelData();

        // Now we need to create a D3D Texture
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = bytes.data();
        // This describes how many bytes wide our image is.
        // Each BGRA pixel is 4 bytes.
        initData.SysMemPitch = width * 4;

        winrt::check_hresult(d3dDevice->CreateTexture2D(&desc, &initData, texture.put()));
    }
    co_return texture;
}

void CopyTexutreIntoCompositionSurface(
    winrt::CompositionDrawingSurface const& surface,
    winrt::com_ptr<ID3D11Texture2D> const& sourceTexture,
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext)
{
    // Since we're going to interop with D3D, we'll need the inteorp COM interface from the surface.
    auto surfaceInterop = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();

    // Make sure our surface is the correct size for our image.
    D3D11_TEXTURE2D_DESC desc = {};
    sourceTexture->GetDesc(&desc);
    winrt::check_hresult(surfaceInterop->Resize({ static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) }));

    // Here we get the underlying D3D texture for our surface. Because composition surfaces come from an
    // atlas, we need to copy our data at an offset. 
    POINT offset = {};
    winrt::com_ptr<ID3D11Texture2D> surfaceTexture;
    winrt::check_hresult(surfaceInterop->BeginDraw(nullptr, winrt::guid_of<ID3D11Texture2D>(), surfaceTexture.put_void(), &offset));
    // Make sure that you call EndDraw when you're finished.
    auto scopeExit = wil::scope_exit([surfaceInterop]()
        {
            winrt::check_hresult(surfaceInterop->EndDraw());
        });

    d3dContext->CopySubresourceRegion(surfaceTexture.get(), 0, offset.x, offset.y, 0, sourceTexture.get(), 0, nullptr);
}

winrt::fire_and_forget LoadImageIntoSurface(
    winrt::CompositionDrawingSurface const& surface, 
    winrt::com_ptr<ID3D11Device> const& d3dDevice)
{
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());

    auto imageTexture = co_await CreateTextureFromImageAsync(d3dDevice);

    CopyTexutreIntoCompositionSurface(surface, imageTexture, d3dContext);
    co_return;
}

int __stdcall WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    // Initialize COM
    winrt::init_apartment();
    
    // Register our window classes
    MainWindow::RegisterWindowClass();

    // Create the DispatcherQueue that the compositor needs to run
    auto controller = util::CreateDispatcherQueueControllerForCurrentThread();

    // Create our window and visual tree
    auto window = MainWindow(L"CompositionImageDemo", 800, 600);
    auto compositor = winrt::Compositor();
    auto target = window.CreateWindowTarget(compositor);
    auto root = compositor.CreateSpriteVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });
    root.Brush(compositor.CreateColorBrush(winrt::Colors::White()));
    target.Root(root);

    // Initialize D3D
    // You could use D2D if you wanted to do more than just load an image.
    // The CreateD3DDevice helper attempts to create a D3D device on hardware before
    // falling back to WARP. You'll need to reason about what's best for your scenario.
    // You can find the code for this helper here: 
    //  https://github.com/robmikh/robmikh.common/blob/bc06cf890e80e4c5e9140b351117bd3abf25d35e/robmikh.common/include/robmikh.common/d3dHelpers.h#L68
    auto d3dDevice = util::CreateD3DDevice();

    // Create the composition surface we'll use for our image. 
    // The CreateCompositionGraphicsDevice helper QIs for ICompositorInterop and calls
    // CreateGraphicsDevice. You can find the code for this helper here:
    //  https://github.com/robmikh/robmikh.common/blob/bc06cf890e80e4c5e9140b351117bd3abf25d35e/robmikh.common/include/robmikh.common/composition.interop.h#L8
    auto compositionGraphics = util::CreateCompositionGraphicsDevice(compositor, d3dDevice.get());
    // We're going to defer the image load, but you could also do it ahead of time.
    // In order to defer it, we create a surface up front with the minimum size. Later
    // we can resize the same surface and dump our pixels into it.
    auto surface = compositionGraphics.CreateDrawingSurface(
        { 1,1 }, 
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 
        winrt::DirectXAlphaMode::Premultiplied);

    // Create the visuals we will use to present the iamge
    auto content = compositor.CreateSpriteVisual();
    content.AnchorPoint({ 0.5f, 0.5f });
    content.RelativeOffsetAdjustment({ 0.5f, 0.5f, 0.0f });
    content.RelativeSizeAdjustment({ 1.0f, 1.0f });
    auto brush = compositor.CreateSurfaceBrush(surface);
    // Here is where we can change things about how the surface is displayed.
    brush.Stretch(winrt::CompositionStretch::None);
    content.Brush(brush);
    root.Children().InsertAtTop(content);

    LoadImageIntoSurface(surface, d3dDevice);
    
    // Message pump
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
