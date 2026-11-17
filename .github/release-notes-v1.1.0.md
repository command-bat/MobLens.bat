## MobLens 1.1.0

### Novidades
- 🪟 **Windows 10, 8.1 e 7**: nas versões anteriores ao Windows 11 o MobLens instala uma webcam DirectShow, vista por Zoom, Discord, Chrome/Edge (Meet), Teams e OBS. No Windows 11 continua a webcam nativa.
- 📐 **Janela redimensionável**: a interface se reorganiza conforme o tamanho (cartões em colunas, depois embaixo da prévia) e, bem pequena, as opções viram um menu ☰.
- 🔭 **Escolha da lente**: escolha o tipo (traseira, frontal) e, ao lado, a lente, com nomes como **Principal**, **Ultra-angular · 0,4x** e **Teleobjetiva · 3x**, calculados pela óptica do celular.
- 🖼️ **Vídeo de espera com a logo**: com o app aberto e o celular desconectado, a webcam mostra a logo do MobLens e o que falta fazer ("Celular não conectado", "Autorize este computador no celular"...).

### Correções
- A webcam ficava sem vídeo após a primeira instalação até o MobLens ser fechado (o app aberto pelo instalador rodava como administrador).
- Textos e controles ficavam com retângulos pretos ao redimensionar a janela.

### Instalação
1. Baixe **`MobLens-Setup.exe`** abaixo e execute (requer administrador).
2. No celular, ative **Opções do desenvolvedor → Depuração USB** e conecte o cabo.
3. Aceite **"Permitir depuração USB"** no celular.
4. No seu app de vídeo, escolha a câmera **MobLens**.

### Requisitos
- Windows 11, 10, 8.1 ou 7 (64 bits)
- Android 12 ou superior
- Driver USB do fabricante do celular (ex.: [Samsung Android USB Driver](https://developer.samsung.com/android-usb-driver))

> ℹ️ No Windows 10 e anteriores, o app **Câmera** do Windows e alguns apps da Microsoft Store não enxergam webcams DirectShow (limitação do sistema), e apenas aplicativos de 64 bits veem a câmera.

> ⚠️ O instalador não é assinado digitalmente. Se o SmartScreen avisar, clique em **Mais informações → Executar assim mesmo**.

### Créditos
Captura no celular feita pelo [`scrcpy-server`](https://github.com/Genymobile/scrcpy) 5.0.1 da Genymobile (Apache 2.0), usado sem modificações.
