/* MobLens - identificadores compartilhados pela DLL e pelo aplicativo. */
#ifndef MOBLENS_IDS_H
#define MOBLENS_IDS_H

/* CLSID da fonte de mídia; também é o "sourceId" do MFCreateVirtualCamera */
#define MOBLENS_CLSID_STR L"{4E8D2A61-93B7-4C5E-A1F2-6B0C9D3E7A58}"

/* Nome exibido na lista de webcams dos aplicativos */
#define MOBLENS_FRIENDLY_NAME L"MobLens"

/* Nome do arquivo da DLL da câmera virtual */
#define MOBLENS_DLL_NAME L"moblens_vcam.dll"

#define MOBLENS_FPS 30u

/* Windows 10/8/7 (sem MFCreateVirtualCamera): webcam DirectShow, carregada
 * dentro de cada aplicativo que usa a câmera */
#define MOBLENS_DSHOW_CLSID_STR L"{B1F4C7D2-6A3E-4E85-9C1B-7D2E5F8A3C64}"
#define MOBLENS_DSHOW_DLL_NAME L"moblens_dshow.dll"

#endif
