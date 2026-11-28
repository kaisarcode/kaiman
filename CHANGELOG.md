# Changelog

## Version 0.1.1

This release makes gateway configuration explicit and inspectable: new services
start without a credential scheme, saved settings are changed with `key=value`,
and both `conf` and `list` report the effective values the gateway will apply
rather than raw empty fields.

- `kaiman add` persists `scheme=none` unless a credential scheme is supplied.
- `kaiman set address=...` and `set port=...` change the saved gateway address
  using `key=value`; a host without a port keeps the saved port. `kaiman conf`
  lists public values and `kaiman start` remains the command that listens.
- `kaiman list` prints effective values for unset fields instead of blanks:
  adapter default URL, `auto`/`local` for local adapters, and `none` for unused
  credential sources. `kc_kaiman_service_effective()` exposes that resolution.
- Documentation and CLI regression coverage reflect these changes.
