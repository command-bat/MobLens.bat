<p align="center">
  <img src="icons/moblens-logo.png" alt="MobLens" width="160">
</p>

<h1 align="center">MobLens</h1>

<p align="center">
  <strong>A câmera do seu celular Android como webcam nativa do Windows 11, via cabo USB.</strong><br>
  Sem OBS, sem câmera virtual de terceiros, sem aplicativo no celular.
</p>

---

## Sumário

- [Visão geral](#visão-geral)
- [O problema que o MobLens resolve](#o-problema-que-o-moblens-resolve)
- [Principais recursos](#principais-recursos)
- [Requisitos](#requisitos)
- [Instalação e uso](#instalação-e-uso)
- [Configurações disponíveis](#configurações-disponíveis)
- [Como funciona](#como-funciona)
- [Compilando a partir do código-fonte](#compilando-a-partir-do-código-fonte)
- [Estrutura do projeto](#estrutura-do-projeto)
- [Solução de problemas](#solução-de-problemas)
- [Créditos e componentes de terceiros](#créditos-e-componentes-de-terceiros)
- [Licenças](#licenças)

---

## Visão geral

O **MobLens** transforma a câmera de um smartphone Android, conectado ao PC por
cabo USB, em um **dispositivo de câmera nativo do Windows 11**, chamado
**"MobLens"**. Para o sistema operacional e para os aplicativos, ele é
indistinguível de uma webcam física: aparece no Google Meet, Microsoft Teams,
Zoom, Discord, navegadores, no aplicativo Câmera do Windows e em qualquer
software que use as APIs de câmera do sistema.

A ideia central do projeto é **fechar o ciclo em uma única solução direta**:

> **celular conectado via USB → reconhecido como webcam nativa no Windows**

Tudo isso em um único aplicativo leve, com interface própria, prévia da câmera
e controles de imagem, que fica discretamente na bandeja do sistema.

## O problema que o MobLens resolve

Os celulares atuais têm câmeras muito superiores às webcams comuns, mas usá-las
no computador costuma ser trabalhoso. As soluções mais comuns:

- exibem a imagem do celular apenas em uma **janela isolada** na área de trabalho;
- exigem instalar **suítes pesadas de transmissão** (como o OBS Studio);
- obrigam o usuário a **capturar a janela manualmente** e ativar uma câmera
  virtual secundária;
- ou dependem de **aplicativos no celular** e conexões Wi-Fi instáveis.

O MobLens elimina todos esses intermediários. A webcam é registrada
diretamente no **Windows Camera Frame Server** por meio da API oficial de
câmeras virtuais do Windows 11 (`MFCreateVirtualCamera`). O resultado é uma
webcam do sistema, com baixa latência e transmissão estável pelo cabo USB.

## Principais recursos

- **Webcam nativa do Windows 11.** Funciona em qualquer aplicativo, sem plugins.
- **Nada para instalar no celular.** O componente de captura é enviado
  automaticamente pelo USB a cada sessão.
- **Liga sob demanda.** A câmera do celular só é ativada quando algum aplicativo
  abre a webcam (ou a janela de prévia está aberta) e desliga sozinha depois.
- **Todas as câmeras do aparelho.** Traseiras (principal, ultra-wide etc.) e frontal.
- **Controles ao vivo.** Zoom e lanterna mudam na hora, sem reiniciar a câmera.
- **Prévia integrada** com indicador de status (Ao vivo, Pronto, Sem celular...).
- **Interface moderna** no estilo do Windows 11, com tema claro e escuro
  automático.
- **Bandeja do sistema.** Todas as opções também ficam acessíveis pelo menu do
  botão direito no ícone.
- **Início com o Windows** (opcional), já escondido na bandeja.
- **Decodificação por hardware** com o decodificador H.264 nativo do Windows
  (Media Foundation).
- **Instalador** com desinstalação limpa pelo Painel de Controle.

## Requisitos

| Item | Requisito |
| :--- | :--- |
| Sistema operacional | Windows 11, 64 bits (a API de câmera virtual não existe no Windows 10) |
| Celular | Android 12 ou superior (captura de câmera do scrcpy-server) |
| Conexão | Cabo USB com **Depuração USB** ativada no celular |
| Driver | Driver USB do fabricante do celular (por exemplo, [Samsung Android USB Driver](https://developer.samsung.com/android-usb-driver)) |

## Instalação e uso

### Pelo instalador

1. Execute **`MobLens-Setup.exe`** e confirme a permissão de administrador.
   O instalador copia o programa para `C:\Program Files\MobLens`, registra a
   webcam virtual e cria atalhos no Menu Iniciar e na Área de Trabalho.
2. No celular, ative **Opções do desenvolvedor → Depuração USB** e conecte o
   cabo. Na primeira conexão, aceite o aviso **"Permitir depuração USB"**.
3. Abra o **MobLens**. A prévia mostra a câmera do celular.
4. No aplicativo de vídeo, escolha a câmera **MobLens**.

> O instalador não possui assinatura digital. Se o Windows SmartScreen exibir
> um aviso, clique em **Mais informações → Executar assim mesmo**.

### Versão portátil

A pasta `dist/MobLens/` também funciona sem instalador: execute
`MobLens.exe`. Na primeira execução, ele pede permissão de administrador para
registrar a webcam virtual.

### Desinstalação

Use **Configurações → Aplicativos → MobLens → Desinstalar**, ou o botão
**Desinstalar webcam virtual** dentro do próprio aplicativo.

## Configurações disponíveis

| Opção | Descrição | Aplicação |
| :--- | :--- | :--- |
| Câmera | Qualquer câmera do celular (traseiras e frontal) | Reinicia a câmera |
| Resolução | Tamanhos suportados pela câmera escolhida (até 4K) | Reinicia a câmera |
| Taxa de quadros | Até 30 fps | Reinicia a câmera |
| Qualidade do vídeo | Taxa de bits de 2 a 24 Mbps | Reinicia a câmera |
| Rotação | 0°, 90°, 180° ou 270° | Reinicia a câmera |
| Zoom | 1,0x a 8,0x | **Ao vivo** |
| Lanterna | Liga o flash (em câmeras que possuem flash) | **Ao vivo** |
| Espelhar imagem | Inverte a imagem horizontalmente no PC | **Ao vivo** |
| Iniciar com o Windows | Inicia escondido na bandeja | — |
| Ao fechar a janela | Esconder na bandeja ou encerrar o aplicativo | — |

As configurações ficam salvas em `HKEY_CURRENT_USER\Software\MobLens`.

## Como funciona

```
 CELULAR (Android)                         PC (Windows 11)
┌──────────────────────┐   USB    ┌──────────────────────┐  memória   ┌────────────────────────┐
│ scrcpy-server        │  (adb)   │ MobLens.exe          │ compartil. │ moblens_vcam.dll       │
│ Camera2 → MediaCodec ├─────────►│ protocolo scrcpy     ├───────────►│ (Windows Frame Server) │
│ H.264                │          │ H.264 → NV12 (MF)    │   NV12     │ IMFMediaSource         │
└──────────────────────┘          └──────────────────────┘            └───────────┬────────────┘
                                                                                  ▼
                                                              Meet · Teams · Zoom · Discord · navegadores
```

1. **Captura (celular).** O MobLens envia o `scrcpy-server` ao celular via
   `adb` e o executa no modo de câmera. Ele captura a imagem com a API Camera2
   e a comprime em H.264 com o codificador de hardware do aparelho.
2. **Transporte (USB).** O vídeo trafega por um túnel `adb reverse` pelo cabo
   USB. Um segundo canal de controle permite alterar zoom e lanterna em tempo
   real.
3. **Decodificação (PC).** O `MobLens.exe` implementa o lado cliente do
   protocolo do scrcpy-server e decodifica o H.264 com o decodificador nativo
   do Windows (Media Foundation), gerando quadros NV12.
4. **Entrega (webcam virtual).** Os quadros são publicados em uma memória
   compartilhada (`Global\MobLensFrameBuffer`, sincronizada por *seqlock*).
   A `moblens_vcam.dll`, carregada pelo serviço **Windows Camera Frame Server**,
   lê esses quadros e os entrega aos aplicativos como uma webcam comum.
5. **Sob demanda.** A DLL sinaliza quando algum aplicativo abre a câmera, e só
   então o celular começa a transmitir. Sem transmissão, a webcam exibe uma
   imagem de espera.

## Compilando a partir do código-fonte

Requer [MSYS2](https://www.msys2.org/). No terminal **MSYS2 UCRT64**:

```bash
cd /d/projetos/mobileCam
./build.sh --deps        # primeira vez: instala GCC, CMake, Ninja e NSIS
./build.sh               # compila em dist/MobLens/
./build.sh --installer   # compila e gera dist/MobLens-Setup.exe
```

O runtime C++ é linkado estaticamente: a DLL roda dentro de um serviço do
sistema, que não enxerga as bibliotecas do MSYS2.

Conteúdo de `dist/MobLens/`:

| Arquivo | Função |
| :--- | :--- |
| `MobLens.exe` | Aplicativo: interface, bandeja, instalação e transmissão |
| `moblens_vcam.dll` | Webcam virtual, carregada pelo Windows Camera Frame Server |
| `adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll` | Comunicação USB com o celular (Android Platform-Tools) |
| `scrcpy-server` | Captura da câmera no celular ([scrcpy](https://github.com/Genymobile/scrcpy) 5.0.1) |
| `LICENSE-scrcpy.txt` | Licença do scrcpy (Apache 2.0) |

## Estrutura do projeto

```
src/
├── app/                    MobLens.exe
│   ├── main.cpp            interface, prévia, bandeja, configurações
│   ├── theme.cpp           tema claro/escuro e controles personalizados
│   ├── streamer.cpp        sessão adb + cliente do protocolo do scrcpy-server
│   ├── h264_decoder.cpp    decodificador H.264 nativo (Media Foundation)
│   ├── nv12.cpp            redimensionamento e espelhamento de quadros NV12
│   ├── adb.cpp             execução do adb em segundo plano
│   ├── installer.cpp       registro da webcam virtual (MFCreateVirtualCamera)
│   └── log.cpp             log de diagnóstico
├── vcam/                   moblens_vcam.dll (IMFMediaSourceEx, IMFMediaStream2, IKsControl)
└── common/                 protocolo da memória compartilhada entre app e DLL
installer/moblens.nsi       script do instalador (NSIS)
icons/                      logotipo e ícone
third_party/scrcpy/         adb e scrcpy-server oficiais, sem modificações
```

## Solução de problemas

| Sintoma | Causa provável e solução |
| :--- | :--- |
| Status "Sem celular" | Depuração USB desligada, cabo só de carga ou falta o driver USB do fabricante. |
| Status "Autorizar no celular" | Aceite o aviso "Permitir depuração USB" na tela do celular. |
| Lanterna não acende | A câmera escolhida não tem flash (comum em lentes ultra-wide e frontais). |
| Webcam não aparece nos apps | Reinstale pelo instalador ou pelo próprio app (requer administrador). |
| Outros erros | Consulte o log em `%LOCALAPPDATA%\MobLens\moblens.log`. |

## Créditos e componentes de terceiros

O MobLens só existe graças ao trabalho destes projetos:

### scrcpy, da Genymobile

**[github.com/Genymobile/scrcpy](https://github.com/Genymobile/scrcpy)**,
licença Apache 2.0.

O MobLens **usa diretamente** componentes do scrcpy:

- **`scrcpy-server` (v5.0.1)**, distribuído **sem modificações**. É o
  componente que roda no celular e captura e codifica a câmera.
- **O protocolo de comunicação do scrcpy.** O `MobLens.exe` contém uma
  implementação própria, em C++, do lado cliente desse protocolo (handshake,
  cabeçalhos de pacotes e mensagens de controle de zoom e lanterna), baseada
  no código e na documentação do scrcpy.
- **A arquitetura geral** (servidor enviado via `adb`, túnel `adb reverse`,
  vídeo H.264 com canal de controle separado) também é inspirada no scrcpy.

O código do cliente original do scrcpy (janela SDL, FFmpeg, entrada de teclado
e mouse) **não** é utilizado.

### Android SDK Platform-Tools (adb), do Google / Android Open Source Project

**[developer.android.com/tools/releases/platform-tools](https://developer.android.com/tools/releases/platform-tools)**,
licença Apache 2.0.

O `adb.exe` e as bibliotecas `AdbWinApi.dll` e `AdbWinUsbApi.dll`, obtidos da
distribuição oficial do scrcpy para Windows, fazem a comunicação USB com o
celular.

### NSIS (Nullsoft Scriptable Install System)

**[nsis.sourceforge.io](https://nsis.sourceforge.io/)**, licença zlib/libpng.

Gera o instalador `MobLens-Setup.exe`.

### Ferramentas de compilação

- **[MSYS2](https://www.msys2.org/)**, **[mingw-w64](https://www.mingw-w64.org/)**
  e **[GCC](https://gcc.gnu.org/)**: compilador e runtime para Windows.
- **[CMake](https://cmake.org/)** e **[Ninja](https://ninja-build.org/)**:
  sistema de build.

### Plataforma Microsoft

O MobLens usa as APIs públicas do Windows: **Media Foundation** (câmera virtual
e decodificador H.264), **Windows Camera Frame Server**, **DWM** e **Common
Controls** (interface). Nenhum código da Microsoft é redistribuído.

## Licenças

- `scrcpy-server`: © Genymobile e Romain Vimont, Apache License 2.0. A licença
  completa acompanha o programa em `LICENSE-scrcpy.txt`.
- `adb` e bibliotecas: © The Android Open Source Project, Apache License 2.0.
- Os nomes e as marcas citados pertencem aos seus respectivos donos. O MobLens
  é um projeto independente, sem afiliação com a Genymobile, o Google ou a
  Microsoft.
