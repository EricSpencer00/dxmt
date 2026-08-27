#pragma once

/* header only: use it as an aggregated object */

#include "dxgi1_2.h"
#include "d3d11_4.h"
#include "log/log.hpp"

namespace dxmt {

/* designed to be used as an aggregated object*/
template <typename IResource> class MTLDXGISurface : public IDXGISurface2 {
public:
  MTLDXGISurface(IResource *pResource) : resource_(pResource) {}
  ~MTLDXGISurface() { ReleaseDeviceContext(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    return resource_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return resource_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return resource_->Release(); }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) final {
    return resource_->SetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *object) final {
    return resource_->SetPrivateDataInterface(guid, object);
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) final {
    return resource_->GetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) final { return GetDevice(riid, parent); }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) final {
    return resource_->GetDeviceInterface(riid, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC1 desc;
    resource_->GetDesc1(&desc);
    pDesc->Width = desc.Width;
    pDesc->Height = desc.Height;
    pDesc->Format = desc.Format;
    pDesc->SampleDesc = desc.SampleDesc;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT *pLockedRect, UINT MapFlags) final {
    ERR_ONCE("DXGISurface::Map: stub");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Unmap() final {
    ERR_ONCE("DXGISurface::Unmap: stub");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetResource(REFIID riid, void **ppParentResource, UINT *pSubresourceIndex) final {
    if (pSubresourceIndex)
      *pSubresourceIndex = 0;
    return resource_->QueryInterface(riid, ppParentResource);
  }

  /* Direct2D's DC render target renders into the texture and then blits out of
   * this DC, so the texture contents have to be visible to GDI. Metal textures
   * are not, hence the round trip through a staging copy and a DIB section. */
  HRESULT STDMETHODCALLTYPE GetDC(BOOL Discard, HDC *phdc) final {
    if (!phdc)
      return E_INVALIDARG;
    *phdc = nullptr;
    if (hdc_)
      return DXGI_ERROR_INVALID_CALL;

    D3D11_TEXTURE2D_DESC1 desc;
    resource_->GetDesc1(&desc);
    if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE))
      return DXGI_ERROR_INVALID_CALL;
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
      return DXGI_ERROR_INVALID_CALL;

    if (FAILED(AcquireDeviceContext()))
      return E_FAIL;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = desc.Width;
    bmi.bmiHeader.biHeight = -(LONG)desc.Height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
      ReleaseDeviceContext();
      return E_FAIL;
    }

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
      DeleteObject(bitmap);
      ReleaseDeviceContext();
      return E_FAIL;
    }

    if (!Discard && FAILED(CopyToBits(desc, bits))) {
      DeleteDC(hdc);
      DeleteObject(bitmap);
      ReleaseDeviceContext();
      return E_FAIL;
    }

    old_bitmap_ = (HBITMAP)SelectObject(hdc, bitmap);
    hdc_ = hdc;
    bitmap_ = bitmap;
    bits_ = bits;
    *phdc = hdc;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReleaseDC(RECT *pDirtyRect) final {
    if (!hdc_)
      return DXGI_ERROR_INVALID_CALL;

    GdiFlush();

    D3D11_TEXTURE2D_DESC1 desc;
    resource_->GetDesc1(&desc);
    HRESULT hr = CopyFromBits(desc, bits_);

    SelectObject(hdc_, old_bitmap_);
    DeleteDC(hdc_);
    DeleteObject(bitmap_);
    hdc_ = nullptr;
    bitmap_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
    ReleaseDeviceContext();
    return hr;
  }

private:
  HRESULT AcquireDeviceContext() {
    if (context_)
      return S_OK;
    Com<ID3D11Device> device;
    HRESULT hr = resource_->GetDeviceInterface(__uuidof(ID3D11Device), (void **)&device);
    if (FAILED(hr))
      return hr;
    device->GetImmediateContext(&context_);
    device_ = std::move(device);
    return context_ ? S_OK : E_FAIL;
  }

  void ReleaseDeviceContext() {
    staging_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
  }

  HRESULT EnsureStaging(const D3D11_TEXTURE2D_DESC1 &desc) {
    if (staging_)
      return S_OK;
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = desc.Width;
    staging_desc.Height = desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    return device_->CreateTexture2D(&staging_desc, nullptr, &staging_);
  }

  HRESULT CopyToBits(const D3D11_TEXTURE2D_DESC1 &desc, void *bits) {
    HRESULT hr = EnsureStaging(desc);
    if (FAILED(hr))
      return hr;
    Com<ID3D11Resource> source;
    if (FAILED(hr = resource_->QueryInterface(__uuidof(ID3D11Resource), (void **)&source)))
      return hr;
    context_->CopySubresourceRegion(staging_.ptr(), 0, 0, 0, 0, source.ptr(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(hr = context_->Map(staging_.ptr(), 0, D3D11_MAP_READ, 0, &mapped)))
      return hr;
    CopyRows((char *)bits, desc.Width * 4, (const char *)mapped.pData, mapped.RowPitch, desc.Width * 4, desc.Height);
    context_->Unmap(staging_.ptr(), 0);
    return S_OK;
  }

  HRESULT CopyFromBits(const D3D11_TEXTURE2D_DESC1 &desc, const void *bits) {
    HRESULT hr = EnsureStaging(desc);
    if (FAILED(hr))
      return hr;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(hr = context_->Map(staging_.ptr(), 0, D3D11_MAP_WRITE, 0, &mapped)))
      return hr;
    CopyRows((char *)mapped.pData, mapped.RowPitch, (const char *)bits, desc.Width * 4, desc.Width * 4, desc.Height);
    context_->Unmap(staging_.ptr(), 0);
    Com<ID3D11Resource> destination;
    if (FAILED(hr = resource_->QueryInterface(__uuidof(ID3D11Resource), (void **)&destination)))
      return hr;
    context_->CopySubresourceRegion(destination.ptr(), 0, 0, 0, 0, staging_.ptr(), 0, nullptr);
    return S_OK;
  }

  static void CopyRows(char *dst, UINT dst_pitch, const char *src, UINT src_pitch, UINT row_bytes, UINT rows) {
    for (UINT y = 0; y < rows; y++)
      memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, row_bytes);
  }

  IResource *resource_; // since it's aggregated, no extra reference is needed
  Com<ID3D11Device> device_;
  Com<ID3D11DeviceContext> context_;
  Com<ID3D11Texture2D> staging_;
  HDC hdc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HBITMAP old_bitmap_ = nullptr;
  void *bits_ = nullptr;
};

} // namespace dxmt
