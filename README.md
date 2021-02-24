# CompositionImageDemo
A minimal sample showing how to load an image into a surface and respond to device lost events using Windows.UI.Composition and Windows.Graphics.Imaging.

The following are highlights from this sample:

## Table of Contents
 * [Image loading](https://github.com/robmikh/CompositionImageDemo#image-loading)
   * [Getting a stream to our image](https://github.com/robmikh/CompositionImageDemo#getting-a-stream-to-our-image)
   * [Decode our image](https://github.com/robmikh/CompositionImageDemo#decode-our-image)
   * [Send our image to the GPU](https://github.com/robmikh/CompositionImageDemo#send-our-image-to-the-gpu)
   * [Copy our texture into a surface](https://github.com/robmikh/CompositionImageDemo#copy-our-texture-into-a-surface)
 * [Responding to a device lost event](https://github.com/robmikh/CompositionImageDemo#responding-to-a-device-lost-event)
   * [Listening for device lost](https://github.com/robmikh/CompositionImageDemo#listening-for-device-lost)
   * [Redrawing our surfaces](https://github.com/robmikh/CompositionImageDemo#redrawing-our-surfaces)

## Image loading

### Getting a stream to our image
We're going to be using Windows.Graphics.Imaging to decode our image, which requires an [`IRandomAccessStream`](https://docs.microsoft.com/en-us/uwp/api/windows.storage.streams.irandomaccessstream?view=winrt-19041) containing the encoded image. In this sample, our image is a local file:

```cpp
auto currentPath = std::filesystem::current_path();
auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(currentPath.wstring());
auto file = co_await folder.GetFileAsync(L"tripphoto1.jpg");
auto stream = co_await file.OpenReadAsync();
```

### Decode our image
Now that we have access to our image, we need to decode it into something we can use on the GPU:

```cpp
// Create the decoder for our image
auto decoder = co_await winrt::BitmapDecoder::CreateAsync(stream);
// Since this image is a jpg it only has a single frame
auto frame = co_await decoder.GetFrameAsync(0);
auto width = frame.PixelWidth();
auto height = frame.PixelHeight();
WINRT_ASSERT(frame.BitmapPixelFormat() == winrt::BitmapPixelFormat::Bgra8);

auto pixelData = co_await frame.GetPixelDataAsync();
auto bytes = pixelData.DetachPixelData();
```

### Send our image to the GPU
In order to get our pixels into a composition surface, the pixels need to first be on the GPU. We'll create a D3D11 texture that contains our decoded image pixels:

```cpp
// Now we need to create a D3D texture
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

winrt::check_hresult(device->CreateTexture2D(&desc, &initData, texture.put()));
```

### Copy our texture into a surface
Composition surfaces are backed by a texture atlas. After we call [`BeginDraw`](https://docs.microsoft.com/en-us/windows/win32/api/windows.ui.composition.interop/nf-windows-ui-composition-interop-icompositiondrawingsurfaceinterop-begindraw), we'll be given an offset into our texture atlas as well as the underlying texture. All that's left is to call [`CopySubresourceRegion`](https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion). Don't forget to call [`EndDraw`](https://docs.microsoft.com/en-us/windows/win32/api/windows.ui.composition.interop/nf-windows-ui-composition-interop-icompositiondrawingsurfaceinterop-enddraw)!

```cpp
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

d3dContext->CopySubresourceRegion(
    surfaceTexture.get(), 
    0, // We only have one subresource
    offset.x, 
    offset.y, 
    0, // z
    sourceTexture.get(), 
    0, // We only have one subresource
    nullptr); // Copy the entire thing
```

## Responding to a device lost event
Sometimes the GPU needs to reset due to events outside of your control. Maybe there's a driver upgrade, maybe someone sent incorrect commands to the GPU, maybe the user is using a Surface Book and just disconnected from their dedicated GPU, etc. It's important that you listen for this event and redraw your surfaces (and other GPU resources) when this happens. Traditionally, applications discover this upon getting an error back from a D3D call. But what if you aren't redrawing your content every frame? What if you had drawn it once and have since moved on, letting Windows.UI.Composition handling the presentation side for you?

### Listening for device lost
Luckily, the [`ID3D11Device4`](https://docs.microsoft.com/en-us/windows/win32/api/d3d11_4/nn-d3d11_4-id3d11device4) interface provides a way to be told when this condition occurs. We can create an NT event and ask our D3D device to signal us when it has gone bad:

```cpp
DWORD cookie = 0;
auto d3dDevice4 = d3dDevice.as<ID3D11Device4>();
winrt::check_hresult(d3dDevice4->RegisterDeviceRemovedEvent(deviceLostEvent.get(), &cookie));
```

Now, whenever your event is signaled you need to create a new D3D device and tell your `CompositionGraphicsDevice` about it. You should also unregister the event from your previous device and register it to your new one. This sample does all this using coroutines:

```cpp
// This sample uses coroutines to wait on the handle without blocking
// the calling thread. This will resume our function on a thread pool thread. 
// The way you handle this event will be up to your application's structure.
co_await winrt::resume_on_signal(deviceLostEvent.get());
deviceLostEvent.ResetEvent(); // Reset the event since we're reusing it.
d3dDevice4->UnregisterDeviceRemoved(cookie);

while (true)
{
    try
    {
        // Create a new D3D device and tell our CompositionGraphicsDevice about it
        auto newD3dDevice = util::CreateD3DDevice();
        RegisterForDeviceLost(deviceLostEvent, newD3dDevice, compGraphics);

        // This will cause the RenderingDeviceReplaced event to fire on the CompositionGraphicsDevice
        auto graphicsDeviceInterop = compGraphics.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>();
        winrt::check_hresult(graphicsDeviceInterop->SetRenderingDevice(newD3dDevice.get()));

        break;
    }
    catch (winrt::hresult_error const& error)
    {
        auto errorCode = error.code();
        if (errorCode == DXGI_ERROR_DEVICE_REMOVED ||
            errorCode == DXGI_ERROR_DEVICE_RESET)
        {
            // Loop around to try again.
        }
        else
        {
            throw;
        }
    }
    // Don't try again too soon.
    co_await std::chrono::milliseconds(500);
}
```

### Redrawing our surfaces
Now that you've replaced your rendering device, the [`RenderingDeviceReplaced`](https://docs.microsoft.com/en-us/uwp/api/windows.ui.composition.compositiongraphicsdevice.renderingdevicereplaced?view=winrt-19041) event will fire. Register for this event before-hand and redraw our surfaces when it fires:

```cpp
// When we get a new D3D device, the RenderingDeviceReplaced event will fire. Here
// we'll register for the event and redraw the surface when it fires. You can 
// exercise this code by using "dxcap.exe -forcetdr". You can get dxcap by going
// to Settings -> Apps -> Optional features -> Graphics Tools. If the image is 
// still there after all the flashing, it worked!
auto eventToken = compositionGraphics.RenderingDeviceReplaced([surface](auto&& compGraphics, auto&&)
    {
        auto graphicsDeviceInterop = compGraphics.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>();
        winrt::com_ptr<IUnknown> unknown;
        winrt::check_hresult(graphicsDeviceInterop->GetRenderingDevice(unknown.put()));
        auto d3dDevice = unknown.as<ID3D11Device>();
        LoadImageIntoSurface(surface, d3dDevice);
    });
```
