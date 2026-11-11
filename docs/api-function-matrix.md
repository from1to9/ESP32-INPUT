# API operation and configuration inventory

The generated [catalog](../api/control-v1.json) lists operation names, argument
bounds, scopes, execution context and configuration fields. The
[OpenAPI definition](../api/openapi.json) lists HTTP resources. The
[MCP tool definitions](../bridge/tools.json) describe the companion bindings.

| Domain | Read first | Related contract |
| --- | --- | --- |
| Input | `/api/v1/state`, `/api/v1/capabilities` | [Input semantics](api-input-semantics.md) |
| Settings | `/api/v1/config`, `/api/v1/config/schema` | [Configuration](configuration.md) |
| Audio and recording | `/api/v1/audio/state`, `/api/v1/audio/capabilities`, `/api/v1/capture/grants` | [Audio and recording](api-audio-recording.md) |
| Connectivity and updates | `/api/v1/wifi/profiles`, `/api/v1/bluetooth/peers`, `/api/v1/firmware/state` | [Wi-Fi and firmware updates](wifi-management-ota.md) |
| Sensors | `/api/v1/sensors/catalog`, `/api/v1/sensors/latest` | [Sensor observations](sensor-api.md) |

Keep API changes in the owning registry and regenerate with
`python3 tools/generate_control_api.py`. Runtime availability depends on
capabilities, permissions and current state. Operations restricted to physical
presence stay local even when their status is visible remotely.
