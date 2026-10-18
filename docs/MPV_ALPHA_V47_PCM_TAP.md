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

Nowa paczka jest publikowana pod grupą `com.github.laszko14` po
udanym buildzie i udostępnieniu Maven przez GitHub Pages. Nie podmieniać
`libplayer.so` bez odpowiadającego mu `libmpv.so`.

## Walidacja nieukończona

- [x] Fork istnieje i jest zapisywalny.
- [x] Patch dopasowany do przypiętego mpv bez fuzz.
- [x] Kod JNI kolejki i API dodany na osobnej gałęzi.
- [ ] Aktywować GitHub Actions w forku (zakładka **Actions** → **I understand my workflows, go ahead and enable them**, jeśli widoczne).
- [ ] GitHub Actions: Android NDK / mpv / `libplayer` / AAR arm64 zakończone sukcesem.
- [ ] Sprawdzić symbole `mpv_rex_set_pcm_sink` w `libmpv.so`.
- [ ] Opublikować artefakt ze spójnymi dwiema bibliotekami i wersją Maven.
- [ ] Zamienić dependency w MPV Alpha i uruchomić CI.
- [ ] Test na telefonie: mowa/cisza/muzyka/szum uliczny, streaming HTTP/HLS/DASH, prędkość 1.5x–3x, wybór ścieżki audio, seek, weryfikacja PTS, brak trzasków.

**Stan:** prototyp połączenia natywnego, nie gotowe wydanie v47. Przy każdym
dalszym PR Auto Audio aktualizować również kanoniczną dokumentację aplikacji.
