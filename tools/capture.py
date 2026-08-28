import asyncio
import time
from datetime import datetime, timezone
from pathlib import Path

import websockets



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
                    receive_time_ns = time.time_ns()

                    file.write(f"{receive_time_ns}\t{message}\n")
                    STATS[key]["message_count"] += 1

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
    write_summary()