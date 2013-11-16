#include <stdint.h>
#include <thumbcache.h>
#include <propsys.h>
#include <shlwapi.h>

class MatroskaThumbnailer : public IThumbnailProvider,
                            public IInitializeWithStream
{
public:
    MatroskaThumbnailer() : reference_count(1), stream(nullptr)
    {
    }

    virtual ~MatroskaThumbnailer()
    {
        if (stream) {
            stream->Release();
        }
    }

    // IUnknown implementation
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(MatroskaThumbnailer, IInitializeWithStream),
            QITABENT(MatroskaThumbnailer, IThumbnailProvider),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&reference_count);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        ULONG cur_ref = InterlockedIncrement(&reference_count);
        if (!cur_ref) {
            delete this;
        }

        return cur_ref;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream *pStream, DWORD grfMode);

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha);

private:
    uint32_t reference_count;
    IStream  *stream;
};

IFACEMETHODIMP MatroskaThumbnailer::Initialize(IStream *pStream, DWORD grfMode)
{
    HRESULT hr = E_UNEXPECTED;

    // Only initialize if the class stream is not yet initialized
    if (!stream) {
        hr = pStream->QueryInterface(&stream);
    }

    return hr;
}

IFACEMETHODIMP MatroskaThumbnailer::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)
{

}