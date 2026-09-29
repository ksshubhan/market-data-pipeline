# Records raw Binance websocket messages for the pipeline to parse and
# replay. One file per stream in captures/, named symbol_market_stream_RUN
# with RUN the start time in UTC; each line is the local receive time in
# nanoseconds, a tab, then the message exactly as received. Runs until
# Ctrl-C, then writes a summary of messages and connections per stream;
# an events log records each connect and disconnect as it happens. Run it
# from the repository root, where captures/ is gitignored.
# src/convert_capture.cpp turns a futures bookTicker file into a .bin.

import asyncio
import time
from datetime import datetime, timezone
from pathlib import Path

import websockets



# Only the two futures bookTicker streams are parsed. Depth is recorded
# but not parsed: it is a stateful stream, kept so that a question about
# one has a file behind it. Spot is outside the futures pipeline's scope.
STREAMS = [
    {
        "symbol": "btcusdt",
        "market": "futures",
        "stream": "bookTicker",
        "url": "wss://fstream.binance.com/public/ws/btcusdt@bookTicker",
    },
    {
        "symbol": "ethwusdt",
        "market": "futures",
        "stream": "bookTicker",
        "url": "wss://fstream.binance.com/public/ws/ethwusdt@bookTicker",
    },
    {
        "symbol": "btcusdt",
        "market": "futures",
        "stream": "depth",
        "url": "wss://fstream.binance.com/public/ws/btcusdt@depth",
    },
    {
        "symbol": "btcusdt",
        "market": "spot",
        "stream": "bookTicker",
        "url": "wss://stream.binance.com:9443/ws/btcusdt@bookTicker",
    },
]

RUN_START_DT = datetime.now(timezone.utc)
RUN_START = RUN_START_DT.strftime("%Y%m%d_%H%M%S")

Path("captures").mkdir(exist_ok=True)

EVENT_LOG = f"captures/capture_{RUN_START}.events.log"

STATS = {}

def write_summary():
    end_time = datetime.now(timezone.utc)
    duration = end_time - RUN_START_DT

    summary_file = f"captures/capture_{RUN_START}.summary.txt"

    with open(summary_file, "w") as file:
        file.write(f"Start time: {RUN_START_DT.isoformat()}\n")
        file.write(f"End time: {end_time.isoformat()}\n")
        file.write(f"Duration: {duration}\n")
        file.write("\n")

        for stats in STATS.values():
            file.write(
                f'{stats["symbol"].upper()} '
                f'{stats["market"]} '
                f'{stats["stream"]}\n'
            )

            file.write(f'Endpoint: {stats["url"]}\n')
            file.write(
                f'Messages: {stats["message_count"]}\n'
            )
            file.write(
                f'Connections: {stats["connection_count"]}\n'
            )
            file.write("\n")

    print(f"Summary written to {summary_file}")

def log_event(message):
    timestamp = datetime.now(timezone.utc).isoformat()

    line = f"{timestamp}\t{message}"

    print(line)

    # Opened and closed per event, so the log is complete even if the
    # process is killed.
    with open(EVENT_LOG, "a") as log:
        log.write(line + "\n")
        log.flush()


async def capture_stream(config):
    symbol = config["symbol"]
    market = config["market"]
    stream = config["stream"]
    url = config["url"]


    key = f"{symbol}_{market}_{stream}"

    STATS[key] = {
        "symbol": symbol,
        "market": market,
        "stream": stream,
        "url": url,
        "message_count": 0,
        "connection_count": 0,
    }


    filename = (
        f"captures/{symbol}_{market}_{stream}_{RUN_START}.log"
    )


    # Opened once for the whole run, so every reconnect appends to the
    # same file with nothing in it to mark the gap; the events log records
    # each reconnect. Iterating websockets.connect opens a new connection
    # each time round, after a close frame or a dropped connection alike.
    with open(filename, "w") as file:
        async for websocket in websockets.connect(url):
            STATS[key]["connection_count"] += 1

            log_event(
                f"CONNECTED {symbol.upper()} {market} {stream} "
                f'connection={STATS[key]["connection_count"]}'
            )
            print(f"Writing to {filename}")

            last_flush = time.monotonic()

            try:
                async for message in websocket:
                    # Wall clock, so the stamp can be compared with
                    # Binance's E and T up to the clock offset, and can
                    # step backwards if NTP corrects it. It is taken when
                    # this coroutine receives the message, after the
                    # library has read and decoded it, on one event loop
                    # shared by every stream.
                    receive_time_ns = time.time_ns()

                    file.write(f"{receive_time_ns}\t{message}\n")
                    STATS[key]["message_count"] += 1

                    # Flushed when a message arrives at least a second
                    # after the last flush, and at every disconnect below;
                    # Ctrl-C closes the file, which flushes it. A killed
                    # process loses what came after the last flush, which
                    # on a quiet stream can be older than a second.
                    if time.monotonic() - last_flush >= 1.0:
                        file.flush()
                        last_flush = time.monotonic()

                        print(
                            f"{symbol.upper()} {market} {stream}: "
                            f'{STATS[key]["message_count"]} messages'
                        )

            except websockets.ConnectionClosed as error:
                log_event(
                    f"DISCONNECTED {symbol.upper()} {market} {stream} "
                    f"code={error.code}"
                )

            else:
                log_event(
                    f"DISCONNECTED {symbol.upper()} {market} {stream} "
                    f"code=normal"
                )

            finally:
                file.flush()

            log_event(
                f"RECONNECTING {symbol.upper()} {market} {stream}"
            )

async def main():
    await asyncio.gather(
        *(capture_stream(config) for config in STREAMS)
    )

try:
    asyncio.run(main())
except KeyboardInterrupt:
    print("\nCapture stopped.")
finally:
    # Written after Ctrl-C or an exception; a killed process writes none.
    write_summary()