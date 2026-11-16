// IMFActivate: objeto COM que o Frame Server instancia pelo CLSID. Ele cria a
// fonte de mídia sob demanda.
#include "vcam.h"

namespace {

class Activator : public IMFActivate {
public:
    Activator() {
        InitializeCriticalSection(&m_cs);
        InterlockedIncrement(&g_objectCount);
    }

    HRESULT Initialize() {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            return hr;
        }
        m_mfStarted = true;
        return MFCreateAttributes(&m_attrs, 8);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IMFAttributes
                || riid == IID_IMFActivate) {
            *ppv = static_cast<IMFActivate *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_ref);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_ref);
        if (!ref) {
            delete this;
        }
        return ref;
    }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        EnterCriticalSection(&m_cs);
        HRESULT hr = S_OK;
        if (!m_source) {
            MediaSource *source = new (std::nothrow) MediaSource();
            if (!source) {
                hr = E_OUTOFMEMORY;
            } else {
                hr = source->Initialize(m_attrs);
                if (SUCCEEDED(hr)) {
                    m_source = source;
                } else {
                    source->Shutdown();
                    source->Release();
                }
            }
        }
        if (SUCCEEDED(hr)) {
            hr = m_source->QueryInterface(riid, ppv);
        }
        LeaveCriticalSection(&m_cs);
        return hr;
    }

    STDMETHODIMP ShutdownObject() override {
        EnterCriticalSection(&m_cs);
        MediaSource *source = m_source;
        m_source = nullptr;
        LeaveCriticalSection(&m_cs);
        if (source) {
            source->Shutdown();
            source->Release();
        }
        return S_OK;
    }

    STDMETHODIMP DetachObject() override {
        EnterCriticalSection(&m_cs);
        MediaSource *source = m_source;
        m_source = nullptr;
        LeaveCriticalSection(&m_cs);
        if (source) {
            source->Release();
        }
        return S_OK;
    }

    // IMFAttributes: delegado ao armazenamento interno
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT *v) override {
        return m_attrs->GetItem(k, v);
    }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE *t) override {
        return m_attrs->GetItemType(k, t);
    }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL *r) override {
        return m_attrs->CompareItem(k, v, r);
    }
    STDMETHODIMP Compare(IMFAttributes *a, MF_ATTRIBUTES_MATCH_TYPE m,
                         BOOL *r) override {
        return m_attrs->Compare(a, m, r);
    }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32 *v) override {
        return m_attrs->GetUINT32(k, v);
    }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64 *v) override {
        return m_attrs->GetUINT64(k, v);
    }
    STDMETHODIMP GetDouble(REFGUID k, double *v) override {
        return m_attrs->GetDouble(k, v);
    }
    STDMETHODIMP GetGUID(REFGUID k, GUID *v) override {
        return m_attrs->GetGUID(k, v);
    }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32 *l) override {
        return m_attrs->GetStringLength(k, l);
    }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32 *l) override {
        return m_attrs->GetString(k, s, n, l);
    }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR *s, UINT32 *l) override {
        return m_attrs->GetAllocatedString(k, s, l);
    }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32 *s) override {
        return m_attrs->GetBlobSize(k, s);
    }
    STDMETHODIMP GetBlob(REFGUID k, UINT8 *b, UINT32 n, UINT32 *s) override {
        return m_attrs->GetBlob(k, b, n, s);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8 **b, UINT32 *s) override {
        return m_attrs->GetAllocatedBlob(k, b, s);
    }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID *p) override {
        return m_attrs->GetUnknown(k, r, p);
    }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override {
        return m_attrs->SetItem(k, v);
    }
    STDMETHODIMP DeleteItem(REFGUID k) override {
        return m_attrs->DeleteItem(k);
    }
    STDMETHODIMP DeleteAllItems() override {
        return m_attrs->DeleteAllItems();
    }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override {
        return m_attrs->SetUINT32(k, v);
    }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override {
        return m_attrs->SetUINT64(k, v);
    }
    STDMETHODIMP SetDouble(REFGUID k, double v) override {
        return m_attrs->SetDouble(k, v);
    }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override {
        return m_attrs->SetGUID(k, v);
    }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override {
        return m_attrs->SetString(k, v);
    }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8 *b, UINT32 s) override {
        return m_attrs->SetBlob(k, b, s);
    }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown *u) override {
        return m_attrs->SetUnknown(k, u);
    }
    STDMETHODIMP LockStore() override {
        return m_attrs->LockStore();
    }
    STDMETHODIMP UnlockStore() override {
        return m_attrs->UnlockStore();
    }
    STDMETHODIMP GetCount(UINT32 *c) override {
        return m_attrs->GetCount(c);
    }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID *k, PROPVARIANT *v) override {
        return m_attrs->GetItemByIndex(i, k, v);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes *dest) override {
        return m_attrs->CopyAllItems(dest);
    }

private:
    ~Activator() {
        // Não desliga a fonte: quem chamou ActivateObject pode continuar
        // usando-a depois de liberar o ativador. Apenas solta a referência.
        DetachObject();
        if (m_attrs) {
            m_attrs->Release();
        }
        if (m_mfStarted) {
            MFShutdown();
        }
        DeleteCriticalSection(&m_cs);
        InterlockedDecrement(&g_objectCount);
    }

    volatile LONG m_ref = 1;
    CRITICAL_SECTION m_cs;
    bool m_mfStarted = false;
    IMFAttributes *m_attrs = nullptr;
    MediaSource *m_source = nullptr;
};

} // namespace

HRESULT CreateActivator(REFIID riid, void **ppv) {
    Activator *activator = new (std::nothrow) Activator();
    if (!activator) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = activator->Initialize();
    if (SUCCEEDED(hr)) {
        hr = activator->QueryInterface(riid, ppv);
    }
    activator->Release();
    return hr;
}
