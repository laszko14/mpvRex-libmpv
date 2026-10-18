# MPV Alpha v47 — PCM tap przed filtrami (projekt natywny)

Repozytorium: https://github.com/laszko14/mpvRex-libmpv
Powiązana aplikacja: https://github.com/laszko14/REX-Player
Kanoniczna dokumentacja: https://github.com/laszko14/REX-Player/blob/master/docs/MPV_Alpha_Auto_Audio_KOMPLET.md

## Cel

Realny VAD podczas odtwarzania bieżącego streamingu bez drugiego dekodera,
pobierania HTTP, mikrofonu, AudioPlaybackCapture, ingerencji w audio użytkownika,
normalizacji oraz zmiany playback speed. Punktem odczytu jest **wyjście dekodera
audio**, przed efektami `af`, RNNoise, EQ, Dialogue Enhance i korekcją tempa.

## Architektura

1. `buildscripts/patches/mpv-vad-pcm-tap.patch` dodaje pasywny filtr do
   `player/audio.c` między `track->dec->f->pins[0]` i wejściem
   `mp_output_chain`. Wszystkie oryginalne `mp_frame` są przekazywane dalej.
2. Eksport `mpv_rex_set_pcm_sink` ustawia callback tylko podczas sesji.
   Przy braku sesji koszt ogranicza się do sprawdzenia wskaźnika; brak JNI.
3. `vad_tap.cpp` składa próbki PCM do niewielkiej statycznej kolejki mono
   S16 (64 × 1024), `try_lock` zamiast czekania w wątku audio. Nie zapisuje
   dźwięku na dysk, nie wysyła go do sieci.
4. `MPVLib.vadTapStart/Stop/Poll` pozwala aplikacji odebrać
   `VadPcmChunk(samples, sampleRate, pts, droppedChunks)`.
   `droppedChunks > 0` **unieważnia VAD**. Nie wolno uzupełniać luk ciszą
   i wyliczać na ich podstawie rozpoznania mowy.
5. Obecna kolejka obsługuje packed/planar signed 16-bit PCM oraz float
   (sprawdzane względem enum formatów przypiętego mpv). Pozostałe formaty
   wymagają konwersji w przyszłej iteracji: do tego czasu brak danych = brak VAD.

## Reproducibility

Źródło mpv przypięte do
`6a262d11f63733d37e3ee6d5fafcb1be1ef83952`.
Błąd dopasowania patcha zatrzymuje build (bez ukrytego fallbacku).
Sprawdzono dopasowanie 4 hunków kodu do dokładnie tej wersji: poprawne.
**To nie zastępuje kompilacji natywnej i testu urządzenia.**

Nowy ARM64-only AAR ma być publikowany jako niezmienny zasób GitHub Release po udanym buildzie na `master`. Aplikacja pobiera kompletny AAR jako zależność Gradle. Nie podmieniać `libplayer.so` bez odpowiadającego mu `libmpv.so`.

## Walidacja nieukończona

- [x] Fork istnieje i jest zapisywalny.
- [x] Patch dopasowany do przypiętego mpv bez fuzz.
- [x] Kod JNI kolejki i API dodany na osobnej gałęzi.
- [x] Aktywowano GitHub Actions w forku.
- [x] Pierwsza kompilacja Android NDK / mpv / `libplayer` / AAR arm64 (run 39159621224) zakończona sukcesem; kolejny ARM64-only build jeszcze do potwierdzenia.
- [x] Potwierdzono symbol `mpv_rex_set_pcm_sink` w ELF `libmpv.so` oraz funkcje JNI VAD w parowanej bibliotece.
- [ ] Opublikować artefakt ze spójnymi dwiema bibliotekami i wersją Maven.
- [ ] Zamienić dependency w MPV Alpha i uruchomić CI.
- [ ] Test na telefonie: mowa/cisza/muzyka/szum uliczny, streaming HTTP/HLS/DASH, prędkość 1.5x–3x, wybór ścieżki audio, seek, weryfikacja PTS, brak trzasków.

**Stan:** prototyp połączenia natywnego, nie gotowe wydanie v47. Przy każdym
dalszym PR Auto Audio aktualizować również kanoniczną dokumentację aplikacji.

## ARM64 only — bieżący etap v47 (18.10.2026)

- Użytkownik zdecydował o **tymczasowym wstrzymaniu ARMv7, x86 i x86_64**. Kod wszystkich ABI pozostaje w repozytorium, ale `.github/workflows/build.yml` uruchamia tylko `arm64`, a `buildscripts/include/ci.sh` buduje jego zależności.
- Potwierdzony poprzedni przebieg [39159621224](https://github.com/laszko14/mpvRex-libmpv/actions/runs/39159621224) miał `success` dla czterech ABI; to weryfikacja podstaw natywnych przed zawężeniem. Pobrano AAR `arm64` o SHA256 `d8b0f8747b8bd754bc492dcdd0b907ff591bb6c0f02b08a093914fd38a95850b` i potwierdzono ELF/JNI (biblioteki `libmpv.so` i `libplayer.so`).
- Po scaleniu PR i *nowym* zielonym buildzie `master`, workflow `.github/workflows/publish-arm64.yml` ma automatycznie opublikować osobny release `native-v47-arm64-r1` wraz z binarnym `mpvRex-libmpv-native-v47-arm64-r1.aar` i `.sha256`. Nie publikować go na podstawie nieudanej kompilacji ani PR z zewnętrznego forka. Nie nadpisywać istniejącego release.
- GitHub Release będzie źródłem zależności ARM64 w aplikacji. **To nie jest Maven GitHub Pages ani APK**; nie zmienia podpisu aplikacji ani jej numeracji. Warunek: kontrola integralności nowego AAR i testy Gradle po stronie `REX-Player`.
- Nowy ARM64-only CI po zmianie skryptów: [39163123352](https://github.com/laszko14/mpvRex-libmpv/actions/runs/39163123352) rozpoczął się; późniejsze zmiany gałęzi mogą mieć nowszy przebieg. Wyniku w tym wpisie nie potwierdzono.
- Prawdziwa skuteczność VAD podczas streamingu, ciągłość PTS i brak zacięć pozostają do sprawdzenia na urządzeniu. Nie wydawać podpisanego v47 przed testem.

