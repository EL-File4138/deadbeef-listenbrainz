# DeaDBeeF ListenBrainz

A Linux plugin for scrobbling from DeaDBeeF to ListenBrainz or a compatible server such as Koito or Maloja. Completed listens are queued locally when the server is unreachable.

## Build and install

Install a C compiler, GNU Make, `pkg-config`, and the development headers for DeaDBeeF, libcurl (≥ 7.85), Jansson, and GLib (≥ 2.66).

```sh
make
make install
```

`make` builds `build/ddb_listenbrainz.so`. `make install` copies it to `~/.local/lib/deadbeef`. To use a different plugin directory, run `make install PLUGINDIR=/path/to/plugins`. Restart DeaDBeeF after installing.

## Configure

Select **ListenBrainz** under **Preferences → Plugins**. Set the API base and user token, then enable scrobbling.

| Server | API base | Compatibility notes |
| --- | --- | --- |
| ListenBrainz | `https://api.listenbrainz.org` | Public service; authenticated submission not tested. |
| Koito | `https://koito.example.org/apis/listenbrainz` | Completed listens tested with 0.3.2. This version has no endpoint to clear now playing on stop. |
| Maloja | `https://maloja.example.org/apis/listenbrainz` | Completed listens tested with 3.2.10. This version does not publish now playing. Leave account trust off: its token check reports the instance name, not an account. |

Use the API base, not the web UI or the `submit-listens` URL. A trailing `/1` is optional. HTTPS uses libcurl's configured CA trust store. The plugin honors DeaDBeeF's proxy settings and does not follow redirects. Leave **Allow unencrypted HTTP** off unless you accept sending the token and track metadata in cleartext. DeaDBeeF stores the token in its secrets file; the queue stores a fingerprint, not the raw token.

## What gets submitted

Tracks need an artist, a title, and a known duration. To qualify, playback must reach half the track or four minutes, whichever comes first. The plugin submits the listen when playback ends or stops and records the original start time. It skips indefinite streams.

Now playing is optional and never queued. **Clear now playing on stop** requests deletion only for this plugin's client name. Turn it off if your server does not support it.

## Queue and token changes

The queue retains up to 1,000 listens for 30 days. The DeaDBeeF log reports when entries expire or the queue drops its oldest entry to make room. Disabling scrobbling pauses capture and delivery without clearing the queue.

To log queue counts, retry delivery, resume a verified backlog, or discard held and rejected listens, select a **Queue operation** in the plugin settings and check **Run queue operation**. Uncheck it before running another operation. Rejected listens do not block the rest of the queue.

Queued listens are tied to the original API base and token. Restoring those settings makes eligible listens deliverable again. To use a new token, both tokens must have been verified as the same account on the same API base. Once the new token is verified, run **Resume backlog for the verified account**. For a compatible server, enable account trust *before* rotating tokens and only if its token check identifies the actual account. Listens never transfer between API bases.

Replay sends one listen at a time by default. Enable batch replay only after verifying that your server handles `import` correctly. If the server accepts a listen but the response is lost, a retry may produce a duplicate.

## Testing

Run `make test` with Python 3.11 or later for the loopback integration tests. Playback and submission were tested with DeaDBeeF 1.10.3 on Linux (plugin API 1.19) and Maloja 3.2.10. Older plugin ABI versions are unverified.

## License

The plugin source and tests are released under [The Unlicense](LICENSE). Third-party dependencies retain their own licenses.
