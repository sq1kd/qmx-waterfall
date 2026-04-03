"""Live IQ spectrum + waterfall viewer from a stereo USB sound device.

Intended for use in Jupyter or as a standalone script.
"""

from __future__ import annotations

import argparse
import queue
from dataclasses import dataclass

import matplotlib.pyplot as plt
import numpy as np
import sounddevice as sd
from matplotlib.animation import FuncAnimation


@dataclass
class ViewerConfig:
    sample_rate: int = 48_000
    block_size: int = 4096
    nfft: int = 8192
    channels: int = 2
    device: int | None = None
    avg_alpha: float = 0.25
    wf_history: int = 250
    db_floor: float = -140.0
    db_ceil: float = 10.0
    interval_ms: int = 40
    cmap: str = "turbo"
    iq_mode: str = "plus"  # plus | minus | swap_plus | swap_minus
    spectrum_mode: str = "welch"  # fft | welch
    welch_nperseg: int = 1024
    welch_overlap: float = 0.5
    display_width_px: int | None = None
    xaxis_0_24khz: bool = False


def _downsample_bins(values: np.ndarray, out_bins: int) -> np.ndarray:
    """Map a spectrum vector to a fixed number of display bins."""
    in_bins = values.shape[0]
    if out_bins <= 0:
        raise ValueError("display_width_px must be positive")
    if out_bins == in_bins:
        return values
    idx = np.linspace(0, in_bins - 1, out_bins).astype(np.int32)
    return values[idx]


def _positive_half_band(values: np.ndarray) -> np.ndarray:
    """Return the positive half of FFT-shifted bins (e.g. 0..24 kHz from 48 kHz Fs)."""
    n = values.shape[0]
    return values[n // 2 :]


def _build_iq(stereo_block: np.ndarray, mode: str) -> np.ndarray:
    i = stereo_block[:, 0]
    q = stereo_block[:, 1]

    if mode == "plus":
        return i + 1j * q
    if mode == "minus":
        return i - 1j * q
    if mode == "swap_plus":
        return q + 1j * i
    if mode == "swap_minus":
        return q - 1j * i
    raise ValueError(f"Unsupported iq_mode: {mode}")


def _compute_fft_db(
    iq: np.ndarray, nfft: int, window: np.ndarray, eps: np.float32
) -> np.ndarray:
    spectrum = np.fft.fftshift(np.fft.fft(iq * window, n=nfft))
    power = np.abs(spectrum) ** 2
    return 10.0 * np.log10(np.maximum(power, eps)).astype(np.float32)


def _compute_welch_db(
    iq: np.ndarray,
    nfft: int,
    welch_nperseg: int,
    welch_overlap: float,
    eps: np.float32,
) -> np.ndarray:
    n = iq.shape[0]
    nperseg = min(welch_nperseg, n)
    noverlap = int(nperseg * welch_overlap)
    step = max(1, nperseg - noverlap)

    welch_window = np.hanning(nperseg).astype(np.float32)
    segments = []
    for start in range(0, n - nperseg + 1, step):
        seg = iq[start : start + nperseg]
        spec = np.fft.fftshift(np.fft.fft(seg * welch_window, n=nfft))
        segments.append(np.abs(spec) ** 2)

    if not segments:
        spec = np.fft.fftshift(np.fft.fft(iq, n=nfft))
        power = np.abs(spec) ** 2
    else:
        power = np.mean(np.stack(segments, axis=0), axis=0)

    return 10.0 * np.log10(np.maximum(power, eps)).astype(np.float32)


def run_live_iq_view(config: ViewerConfig = ViewerConfig(), show_plot: bool = True):
    """Start live spectrum + waterfall UI.

    Returns a tuple: (stream, animation, figure)
    Keep references alive while running.
    """

    if config.channels < 2:
        raise ValueError("channels must be at least 2 for I/Q capture")
    if not (0.0 < config.avg_alpha <= 1.0):
        raise ValueError("avg_alpha must be in (0, 1]")
    if config.nfft <= 0 or config.block_size <= 0:
        raise ValueError("nfft and block_size must be positive")
    if config.spectrum_mode not in {"fft", "welch"}:
        raise ValueError("spectrum_mode must be 'fft' or 'welch'")
    if config.welch_nperseg <= 0:
        raise ValueError("welch_nperseg must be positive")
    if not (0.0 <= config.welch_overlap < 1.0):
        raise ValueError("welch_overlap must be in [0, 1)")
    if config.display_width_px is not None and config.display_width_px <= 0:
        raise ValueError("display_width_px must be positive")

    audio_q: queue.Queue[np.ndarray] = queue.Queue(maxsize=8)
    window = np.hanning(config.block_size).astype(np.float32)
    eps = np.float32(1e-12)

    freq = np.fft.fftshift(np.fft.fftfreq(config.nfft, d=1.0 / config.sample_rate))
    freq_khz = freq / 1000.0
    fs_khz = config.sample_rate / 1000.0

    display_bins = config.display_width_px or config.nfft
    if config.xaxis_0_24khz:
        # Show only the positive half-band and label it as -Fs/4..Fs/4.
        # For Fs=48 kHz this is -12..12 kHz.
        freq_khz_display = np.linspace(-fs_khz / 4.0, fs_khz / 4.0, display_bins)
    else:
        # Shift axis by -Fs/4 (e.g. -36..12 kHz for 48 kHz sampling).
        freq_khz_display = np.linspace(
            freq_khz[0] - fs_khz / 4.0, freq_khz[-1] - fs_khz / 4.0, display_bins
        )

    spec_ema = np.full(display_bins, config.db_floor, dtype=np.float32)
    wf = np.full((config.wf_history, display_bins), config.db_floor, dtype=np.float32)
    wf_write = 0

    def audio_callback(indata, _frames, _time_info, _status):
        block = indata.copy()
        try:
            audio_q.put_nowait(block)
        except queue.Full:
            try:
                audio_q.get_nowait()
            except queue.Empty:
                pass
            try:
                audio_q.put_nowait(block)
            except queue.Full:
                pass

    fig, (ax1, ax2) = plt.subplots(
        2,
        1,
        figsize=(10, 7),
        gridspec_kw={"height_ratios": [1.0, 1.2]},
    )

    (line,) = ax1.plot(freq_khz_display, spec_ema, lw=1)
    ax1.set_xlim(freq_khz_display[0], freq_khz_display[-1])
    ax1.set_ylim(config.db_floor, config.db_ceil)
    ax1.set_xlabel("Frequency (kHz)")
    ax1.set_ylabel("Level (dB)")
    ax1.set_title(
        f"Live IQ Spectrum ({config.spectrum_mode.upper()}, {display_bins} bins)"
    )
    ax1.grid(alpha=0.3)

    img = ax2.imshow(
        wf,
        aspect="auto",
        cmap=config.cmap,
        vmin=config.db_floor,
        vmax=config.db_ceil,
        extent=[freq_khz_display[0], freq_khz_display[-1], 0, config.wf_history],
        origin="upper",
        interpolation="nearest",
    )
    ax2.set_xlabel("Frequency (kHz)")
    ax2.set_ylabel("Time (rows)")
    ax2.set_title("Live Waterfall (newest at bottom)")
    cbar = fig.colorbar(img, ax=ax2, pad=0.01)
    cbar.set_label("dB")
    plt.tight_layout()

    def update(_frame_idx):
        nonlocal spec_ema, wf_write, wf

        latest = None
        while True:
            try:
                latest = audio_q.get_nowait()
            except queue.Empty:
                break

        if latest is None:
            return line, img

        iq = _build_iq(latest, config.iq_mode)
        if config.spectrum_mode == "welch":
            db = _compute_welch_db(
                iq,
                nfft=config.nfft,
                welch_nperseg=config.welch_nperseg,
                welch_overlap=config.welch_overlap,
                eps=eps,
            )
        else:
            db = _compute_fft_db(iq, nfft=config.nfft, window=window, eps=eps)

        if config.xaxis_0_24khz:
            db_view = _positive_half_band(db)
        else:
            db_view = db

        db_display = _downsample_bins(db_view, display_bins)

        spec_ema = (1.0 - config.avg_alpha) * spec_ema + config.avg_alpha * db_display
        line.set_ydata(spec_ema)

        wf[wf_write, :] = db_display
        wf_write = (wf_write + 1) % config.wf_history

        if wf_write == 0:
            wf_view = wf
        else:
            wf_view = np.vstack((wf[wf_write:, :], wf[:wf_write, :]))
        img.set_data(wf_view)

        return line, img

    # If no explicit device is provided, pick the current default input.
    # This avoids ambiguous tuple defaults and keeps behavior predictable.
    device = config.device
    if device is None:
        default_in, _default_out = sd.default.device
        if default_in is not None and int(default_in) >= 0:
            device = int(default_in)

    stream = sd.InputStream(
        samplerate=config.sample_rate,
        blocksize=config.block_size,
        channels=config.channels,
        dtype="float32",
        device=device,
        callback=audio_callback,
    )
    stream.start()

    animation = FuncAnimation(
        fig,
        update,
        interval=config.interval_ms,
        blit=False,
        cache_frame_data=False,
    )
    animation.event_source.start()
    fig.canvas.draw_idle()
    # Keep strong references attached to the figure so notebook backends
    # don't garbage-collect the live stream/animation unexpectedly.
    fig._live_stream = stream
    fig._live_animation = animation
    if show_plot:
        plt.show()
    return stream, animation, fig


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Live IQ spectrum + waterfall viewer")
    parser.add_argument(
        "--list-devices", action="store_true", help="List sound devices and exit"
    )
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--block-size", type=int, default=4096)
    parser.add_argument("--nfft", type=int, default=8192)
    parser.add_argument("--device", type=int, default=None)
    parser.add_argument("--avg-alpha", type=float, default=0.25)
    parser.add_argument("--wf-history", type=int, default=250)
    parser.add_argument("--db-floor", type=float, default=-140.0)
    parser.add_argument("--db-ceil", type=float, default=10.0)
    parser.add_argument("--interval-ms", type=int, default=40)
    parser.add_argument(
        "--iq-mode",
        choices=["plus", "minus", "swap_plus", "swap_minus"],
        default="plus",
        help="How to combine stereo channels into complex IQ",
    )
    parser.add_argument(
        "--spectrum-mode",
        choices=["fft", "welch"],
        default="fft",
        help="FFT magnitude or Welch PSD estimate",
    )
    parser.add_argument("--welch-nperseg", type=int, default=1024)
    parser.add_argument("--welch-overlap", type=float, default=0.5)
    parser.add_argument(
        "--display-width-px",
        type=int,
        default=None,
        help="Horizontal display bins (e.g. 284 to simulate target LCD width)",
    )
    parser.add_argument(
        "--esp32s3-profile",
        action="store_true",
        help="Use lower-compute defaults better aligned with ESP32S3 constraints",
    )
    parser.add_argument(
        "--xaxis-0-24khz",
        "--xaxis-0-24kHz",
        action="store_true",
        help=(
            "Show only positive 24 kHz of spectrum and label X axis as -12..12 kHz "
            "(at 48 kHz sample rate)"
        ),
    )
    return parser


def main() -> None:
    parser = _build_parser()
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    if args.esp32s3_profile:
        # if args.block_size == 4096:
        # args.block_size = 1024
        if args.nfft == 8192:
            args.nfft = 512
        if args.display_width_px is None:
            args.display_width_px = 284
        if args.wf_history == 250:
            args.wf_history = 50

    cfg = ViewerConfig(
        sample_rate=args.sample_rate,
        block_size=args.block_size,
        nfft=args.nfft,
        device=args.device,
        avg_alpha=args.avg_alpha,
        wf_history=args.wf_history,
        db_floor=args.db_floor,
        db_ceil=args.db_ceil,
        interval_ms=args.interval_ms,
        iq_mode=args.iq_mode,
        spectrum_mode=args.spectrum_mode,
        welch_nperseg=args.welch_nperseg,
        welch_overlap=args.welch_overlap,
        display_width_px=args.display_width_px,
        xaxis_0_24khz=args.xaxis_0_24khz,
    )

    stream, _ani, _fig = run_live_iq_view(cfg)
    try:
        plt.show(block=True)
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop()
        stream.close()


if __name__ == "__main__":
    main()
