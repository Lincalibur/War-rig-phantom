// CyberDeck orchestrator controller — Flutter + flutter_blue_plus
//
// Scans for the C3 by our custom SERVICE UUID (the advertised name is blank, so we match on the
// UUID instead), connects, bonds in the background (passkey 123456), subscribes to STATUS, and
// exposes the mode grid. WiFi-group modes intentionally drop the BLE link on the C3 (single-radio
// handoff) — the app treats that disconnect as expected and offers a one-tap reconnect.
//
// Setup (see README notes in chat): flutter create this folder, overwrite pubspec.yaml + lib/main.dart,
// add the BLE permissions to AndroidManifest.xml, set minSdkVersion 21+, then `flutter run`.

import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

// ---- Protocol (must match orchestrator/v1/c3/c3.ino) ----
const String kSvcUuid  = '6e40a000-b5a3-f393-e0a9-e50e24dcca9e';
const String kCmdUuid  = '6e40a001-b5a3-f393-e0a9-e50e24dcca9e';
const String kStatUuid = '6e40a002-b5a3-f393-e0a9-e50e24dcca9e';
const String kDataUuid = '6e40a003-b5a3-f393-e0a9-e50e24dcca9e';

const int kOpStop = 0xFF;           // STOP -> IDLE
const int kOpClockSync = 0xF0;
const int kOpSetTarget = 0x20;      // [0x20, b0..b5, ch, ...ssid]
const int kOpScanTargets = 0x21;    // brief WiFi scan, then auto-returns to IDLE
const int kOpClearTarget = 0x22;
const int kOpSendAplist = 0x23;     // pull the scanned AP list over DATA

class DeckMode {
  final String name;
  final int opcode;
  final bool dropsBle;        // true = WiFi-group mode, BLE link will drop
  final bool isTx;            // true = actively transmits
  final bool targetable;      // shows the target section; uses the locked target
  final bool requiresTarget;  // Run is blocked until a target is set (deauth)
  const DeckMode(this.name, this.opcode,
      {this.dropsBle = false, this.isTx = false, this.targetable = false, this.requiresTarget = false});
}

// A WiFi network the user can pick as a target (parsed from the C3's DATA stream).
class ApTarget {
  final String bssid;   // "AA:BB:CC:DD:EE:FF"
  final int ch;
  final int rssi;
  final String ssid;
  const ApTarget(this.bssid, this.ch, this.rssi, this.ssid);
}

// Grouped to match the firmware's mode set.
const List<DeckMode> kWifiModes = [
  DeckMode('Passive Recon', 0x00, dropsBle: true),
  DeckMode('WiFi Scan', 0x01, dropsBle: true),
  DeckMode('Deauth', 0x02, dropsBle: true, isTx: true, targetable: true, requiresTarget: true),
  DeckMode('Beacon Spam', 0x03, dropsBle: true, isTx: true),
  DeckMode('Probe Flood', 0x04, dropsBle: true, isTx: true),
  DeckMode('Karma', 0x05, dropsBle: true, isTx: true, targetable: true),
  DeckMode('Handshake Capture', 0x06, dropsBle: true, targetable: true),
  DeckMode('Evil Portal', 0x07, dropsBle: true, isTx: true),
];
const List<DeckMode> kBleModes = [
  DeckMode('BLE Scan', 0x10),
  DeckMode('AirTag Scan', 0x11),
  DeckMode('Flipper Scan', 0x12),
  DeckMode('Skimmer Scan', 0x13),
  DeckMode('BLE Spam', 0x14, isTx: true),
  // HID Trap turns the C3 into a BLE keyboard, so the control link drops like a WiFi handoff.
  DeckMode('BLE HID Trap', 0x15, dropsBle: true, isTx: true),
];

// Ducky scripts embedded in the C3 firmware (orchestrator/v1/c3/ducky.h) — keep this list in the
// same order as DUCKY[] there; the index is sent as byte[1] of the HID-trap command (0x15).
const int kOpHidTrap = 0x15;
const List<String> kDuckyScripts = [
  'Rickroll — KDE/Kubuntu',
  'Rickroll — Windows',
  'Notepad Hello — Win',
  'Fake Update — Win',
  'Lock Screen — Win',
  'Task Manager — Win',
  'Hacker CMD — Win',
];

// Per-mode briefing shown on the full-screen detail page (keyed by opcode).
class ModeInfo {
  final String what;   // what it does
  final String how;    // how it works
  const ModeInfo(this.what, this.how);
}

const Map<int, ModeInfo> kModeInfo = {
  0x00: ModeInfo(
    'Listens to the air and maps nearby WiFi — access points and the client devices talking to them — without transmitting anything.',
    'The C3 puts its radio in promiscuous (monitor) mode and hops channels 1–13. It parses beacon, probe and data frames to learn each AP\'s SSID, channel and encryption, and watches which clients follow which networks. 100% passive — nothing is sent, so it is undetectable.'),
  0x01: ModeInfo(
    'A quick access-point survey: lists every WiFi network in range, strongest first, with channel and security type.',
    'Same promiscuous sniff as Passive Recon but focused on the AP table only. OPEN/WEP networks are flagged red/orange as weak. Read-only — no packets are transmitted.'),
  0x02: ModeInfo(
    'Knocks clients off ONE chosen access point by forging "you are disconnected" frames. Requires a target — OWN-LAB ONLY.',
    'You must pick a target network first (scan and select it, or type its BSSID). The C3 then injects 802.11 deauth + disassociation frames (reason 7) to that one AP\'s BSSID on its channel — nothing else is touched. With no target set it transmits NOTHING, so you can\'t clip a neighbour by accident. Using it on networks you don\'t own is illegal.'),
  0x03: ModeInfo(
    'Floods the area with fake WiFi network names so lots of bogus SSIDs appear in everyone\'s WiFi list. OWN-LAB ONLY.',
    'The C3 rapidly transmits forged beacon frames, each with a random BSSID and a rotating fake SSID, across the channel hop set. Scanning devices believe dozens of networks exist. Pure noise generation for testing scanners.'),
  0x04: ModeInfo(
    'Impersonates many client devices all searching for networks at once, flooding the air with probe requests. OWN-LAB ONLY.',
    'The C3 injects 802.11 probe-request frames from randomised source MACs asking for a rotating list of SSIDs. Useful for stress-testing WIDS/monitoring that watches for client probes.'),
  0x05: ModeInfo(
    'Lures devices by advertising your chosen network name back out, so a client that knows it will try to join. OWN-LAB ONLY.',
    'Set a target first — Karma only beacons that one SSID (your own network\'s name) as bait. For safety it never re-broadcasts the real SSIDs it overhears nearby clients asking for, so it can\'t accidentally impersonate a neighbour\'s network. With no target it just emits made-up placeholder names.'),
  0x06: ModeInfo(
    'Waits to catch a WPA2 4-way handshake — the encrypted login exchange — when a device joins a network.',
    'In promiscuous mode the C3 watches data frames for EAPOL packets (ethertype 0x888E) and counts them, remembering which AP they belong to. Tip: running Deauth briefly forces a client to reconnect, which replays the handshake. Full capture-to-phone (pcap) is the next milestone — for now it counts and identifies them.'),
  0x07: ModeInfo(
    'Stands up a rogue open WiFi hotspot that redirects anything that joins to a test notice page. OWN-LAB ONLY.',
    'The C3 becomes an open SoftAP ("Free Airport WiFi"), runs a tiny DNS server that answers every lookup with its own address, and serves one plain "this is a lab test AP" page to every request. It demonstrates the rogue-AP + captive-redirect mechanics — it does NOT present a login form or collect anything.'),
  0x10: ModeInfo(
    'Scans for every Bluetooth LE device nearby and lists them by signal strength.',
    'The C3 runs an active BLE scan alongside the phone link (the app stays connected). Each advertising device is shown with its RSSI and name/address. Great for seeing what\'s beaconing around you.'),
  0x11: ModeInfo(
    'Hunts for Apple Find-My / AirTag trackers that may be following you.',
    'A filtered BLE scan: the C3 inspects manufacturer data for Apple\'s company ID (0x004C) with the offline-finding type (0x12) that AirTags and Find-My devices broadcast. Matches are flagged so you can spot an unexpected tracker.'),
  0x12: ModeInfo(
    'Detects Flipper Zero devices advertising over Bluetooth nearby.',
    'A filtered BLE scan matching advertising names containing "Flipper". Shows signal strength so you can tell how close one is.'),
  0x13: ModeInfo(
    'Looks for the cheap Bluetooth-serial modules commonly built into card skimmers.',
    'A filtered BLE scan matching the device names those modules ship with (HC-05/06/03, AT-09, MLT-BT05, JDY…). A hit at a fuel pump or ATM is worth a second look. Heuristic — these chips have legitimate uses too.'),
  0x14: ModeInfo(
    'Spams nearby phones with Bluetooth pairing pop-ups. OWN-LAB ONLY.',
    'The C3 rapidly rotates its BLE advertisements through forged proximity-pairing payloads, which makes "connect?" pop-ups appear on nearby devices. The phone\'s control link stays connected throughout. For testing your own devices only.'),
  0x15: ModeInfo(
    'Pretends to be a Bluetooth keyboard; once a computer pairs, it auto-types a chosen script (e.g. a rickroll). OWN-LAB ONLY.',
    'The C3 advertises as a HID keyboard ("Wireless Keyboard"). When your target PC pairs, it waits ~2.5s then types the selected DuckyScript as if someone were at the keyboard. Pick the script below — the KDE/Kubuntu rickroll is the default for your laptop.'),
};

void main() => runApp(const CyberDeckApp());

class CyberDeckApp extends StatelessWidget {
  const CyberDeckApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CyberDeck',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorScheme: ColorScheme.fromSeed(
            seedColor: const Color(0xFF00E5FF), brightness: Brightness.dark),
      ),
      home: const HomePage(),
    );
  }
}

enum Conn { off, scanning, connecting, bonding, ready, disconnected }

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  Conn _conn = Conn.off;
  String _status = 'Not connected';
  String _deckStatus = '';
  BluetoothDevice? _device;
  BluetoothCharacteristic? _cmd;
  BluetoothCharacteristic? _stat;
  BluetoothCharacteristic? _data;
  bool _expectDrop = false;              // true after we send a WiFi mode (handoff drop is expected)

  // Attack target (safety). null = no target; active attacks that require one are blocked.
  final ValueNotifier<ApTarget?> _target = ValueNotifier(null);
  final ValueNotifier<List<ApTarget>> _aps = ValueNotifier([]);   // networks from the last target scan
  final ValueNotifier<bool> _scanBusy = ValueNotifier(false);
  String _dataBuf = '';
  bool _scanningTargets = false;         // true during a 0x21 scan (auto-reconnect, no BOOT needed)
  bool _pullAplistOnReady = false;       // after reconnect, pull the scan list with 0x23

  final List<StreamSubscription> _subs = [];
  StreamSubscription? _connSub;          // the active connection-state listener

  @override
  void initState() {
    super.initState();
    _boot();
  }

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    _connSub?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  Future<void> _boot() async {
    if (Platform.isAndroid) {
      await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.locationWhenInUse,
      ].request();
    }
    // Make sure the adapter is on before scanning.
    if (await FlutterBluePlus.adapterState.first != BluetoothAdapterState.on) {
      _set(Conn.off, 'Turn Bluetooth on, then tap Scan');
      return;
    }
    _startScan();
  }

  void _set(Conn c, String s) {
    if (!mounted) return;
    setState(() { _conn = c; _status = s; });
  }

  Future<void> _startScan() async {
    _set(Conn.scanning, 'Scanning for CyberDeck…');
    try {
      _subs.add(FlutterBluePlus.scanResults.listen((results) {
        for (final r in results) {
          _onFound(r.device);
          break;
        }
      }));
      await FlutterBluePlus.startScan(
        withServices: [Guid(kSvcUuid)],
        timeout: const Duration(seconds: 20),
      );
      // If the scan times out with nothing found:
      if (_conn == Conn.scanning) {
        _set(Conn.off, 'No CyberDeck found. Is it on and in IDLE?');
      }
    } catch (e) {
      _set(Conn.off, 'Scan error: $e');
    }
  }

  Future<void> _onFound(BluetoothDevice device) async {
    if (_conn == Conn.connecting || _conn == Conn.ready) return;
    await FlutterBluePlus.stopScan();
    _device = device;
    _set(Conn.connecting, 'Connecting…');
    try {
      _connSub?.cancel();
      _connSub = device.connectionState.listen((s) {
        if (s != BluetoothConnectionState.disconnected) return;
        if (_scanningTargets) {
          // Target scan drops BLE, then the C3 auto-returns after ~8s — reconnect ourselves.
          _set(Conn.connecting, 'Scanning networks… reconnecting shortly');
          Future.delayed(const Duration(seconds: 9), () {
            if (mounted) { _scanningTargets = false; _reconnect(); }
          });
        } else if (_expectDrop) {
          _set(Conn.disconnected,
              'WiFi mode running — BLE dropped by the handoff. Press the C3 BOOT button, then Reconnect.');
        } else if (_conn == Conn.ready) {
          // Unexpected drop: the deck is still advertising, so pull it back automatically.
          _set(Conn.connecting, 'Link dropped — reconnecting…');
          Future.delayed(const Duration(milliseconds: 800), () {
            if (mounted && _conn == Conn.connecting) _reconnect();
          });
        }
      });
      await device.connect(timeout: const Duration(seconds: 15));
      await _discoverAndBond(device);
    } catch (e) {
      _set(Conn.disconnected, 'Connect failed: $e');
    }
  }

  Future<void> _discoverAndBond(BluetoothDevice device) async {
    // Trigger bonding up front so the system pairing dialog (passkey 123456) appears in the
    // background rather than mid-command.
    if (Platform.isAndroid && device.bondState.first != BluetoothBondState.bonded) {
      _set(Conn.bonding, 'Pairing (enter 123456 if prompted)…');
      try { await device.createBond(); } catch (_) {/* may already be bonding */}
    }

    // Bump the MTU so STATUS strings and the DATA target-list lines aren't truncated at 20 bytes.
    if (Platform.isAndroid) { try { await device.requestMtu(185); } catch (_) {} }

    final services = await device.discoverServices();
    for (final svc in services) {
      if (svc.uuid != Guid(kSvcUuid)) continue;
      for (final c in svc.characteristics) {
        if (c.uuid == Guid(kCmdUuid)) _cmd = c;
        if (c.uuid == Guid(kStatUuid)) _stat = c;
        if (c.uuid == Guid(kDataUuid)) _data = c;
      }
    }
    if (_cmd == null || _stat == null) {
      _set(Conn.disconnected, 'CyberDeck service not found on device');
      return;
    }

    // Subscribe to STATUS (this forces the encrypted link / pairing if not already bonded).
    try {
      await _stat!.setNotifyValue(true);
      _subs.add(_stat!.lastValueStream.listen((v) {
        if (v.isEmpty) return;
        if (!mounted) return;
        setState(() => _deckStatus = utf8.decode(v, allowMalformed: true));
      }));
    } catch (e) {
      _set(Conn.disconnected, 'Could not subscribe (bond failed?): $e');
      return;
    }

    // Subscribe to DATA — the C3 streams the scanned AP list here (one line per network, then "END").
    if (_data != null) {
      try {
        await _data!.setNotifyValue(true);
        _subs.add(_data!.lastValueStream.listen((v) {
          if (v.isEmpty) return;
          _dataBuf += utf8.decode(v, allowMalformed: true);
          int nl;
          while ((nl = _dataBuf.indexOf('\n')) >= 0) {
            final line = _dataBuf.substring(0, nl).trim();
            _dataBuf = _dataBuf.substring(nl + 1);
            _onDataLine(line);
          }
        }));
      } catch (_) {}
    }

    _expectDrop = false;
    _syncClock();
    _set(Conn.ready, 'Connected');

    // If we just came back from a target scan, pull the collected network list.
    if (_pullAplistOnReady) { _pullAplistOnReady = false; _pullAplist(); }
  }

  void _onDataLine(String line) {
    if (line.isEmpty) return;
    if (line == 'END') { _scanBusy.value = false; return; }
    final p = line.split('\t');
    if (p.length < 4) return;
    final ap = ApTarget(p[0], int.tryParse(p[1]) ?? 0, int.tryParse(p[2]) ?? 0, p[3]);
    _aps.value = [..._aps.value, ap];
  }

  Future<void> _syncClock() async {
    if (_cmd == null) return;
    final epoch = DateTime.now().millisecondsSinceEpoch ~/ 1000;
    final payload = [
      kOpClockSync,
      epoch & 0xFF, (epoch >> 8) & 0xFF, (epoch >> 16) & 0xFF, (epoch >> 24) & 0xFF,
    ];
    try { await _cmd!.write(payload, withoutResponse: false); } catch (_) {}
  }

  Future<void> _send(DeckMode m) async {
    if (_cmd == null) return;
    _expectDrop = m.dropsBle;           // a WiFi mode will drop BLE on purpose
    try {
      await _cmd!.write([m.opcode], withoutResponse: false);
      if (m.dropsBle) {
        _set(Conn.disconnected,
            '${m.name} running — BLE dropped by the handoff. Press the C3 BOOT button, then Reconnect.');
      } else {
        _snack('Sent: ${m.name}');
      }
    } catch (e) {
      _snack('Write failed: $e');
    }
  }

  // Open the full-screen briefing for a mode; its Run button calls back here to fire.
  void _openDetail(DeckMode m) {
    Navigator.push(context, MaterialPageRoute(
      builder: (_) => ModeDetailPage(
        mode: m,
        target: _target,
        onChooseTarget: m.targetable ? _openTargetPage : null,
        onRun: (scriptIdx) => m.opcode == kOpHidTrap ? _sendHid(scriptIdx) : _send(m),
      ),
    ));
  }

  void _openTargetPage() {
    Navigator.push(context, MaterialPageRoute(
      builder: (_) => TargetPage(
        aps: _aps, target: _target, busy: _scanBusy,
        onScan: _scanTargets, onPick: _setTarget, onClear: _clearTarget,
      ),
    ));
  }

  // ---- Targeting (safety) ------------------------------------------------------------
  // Kick off an ~8s WiFi scan on the C3. It drops BLE, collects networks, auto-returns to IDLE,
  // and we reconnect + pull the list. BLE drop here is expected and self-heals (no BOOT press).
  Future<void> _scanTargets() async {
    if (_cmd == null) return;
    _scanningTargets = true;
    _pullAplistOnReady = true;
    _expectDrop = true;
    _scanBusy.value = true;
    _aps.value = [];
    try {
      await _cmd!.write([kOpScanTargets], withoutResponse: false);
    } catch (e) {
      _scanningTargets = false; _scanBusy.value = false; _snack('Scan failed: $e');
    }
  }

  Future<void> _pullAplist() async {
    if (_cmd == null) return;
    _aps.value = [];
    _scanBusy.value = true;
    try { await _cmd!.write([kOpSendAplist], withoutResponse: false); } catch (_) {}
    Future.delayed(const Duration(seconds: 6), () => _scanBusy.value = false);   // safety timeout
  }

  Future<void> _setTarget(ApTarget ap) async {
    if (_cmd == null) return;
    final b = _parseBssid(ap.bssid);
    if (b == null) { _snack('Bad BSSID — need AA:BB:CC:DD:EE:FF'); return; }
    final payload = [kOpSetTarget, ...b, ap.ch & 0xFF, ...utf8.encode(ap.ssid)];
    try {
      await _cmd!.write(payload, withoutResponse: false);
      _target.value = ap;
      _snack('Target set: ${ap.ssid.isEmpty ? ap.bssid : ap.ssid}');
    } catch (e) { _snack('Set target failed: $e'); }
  }

  Future<void> _clearTarget() async {
    if (_cmd == null) return;
    try { await _cmd!.write([kOpClearTarget], withoutResponse: false); } catch (_) {}
    _target.value = null;
    _snack('Target cleared');
  }

  List<int>? _parseBssid(String s) {
    final parts = s.split(RegExp(r'[:\-\s]+')).where((e) => e.isNotEmpty).toList();
    if (parts.length != 6) return null;
    final out = <int>[];
    for (final p in parts) {
      final v = int.tryParse(p, radix: 16);
      if (v == null || v < 0 || v > 255) return null;
      out.add(v);
    }
    return out;
  }

  Future<void> _sendHid(int idx) async {
    if (_cmd == null) return;
    _expectDrop = true;
    try {
      await _cmd!.write([kOpHidTrap, idx], withoutResponse: false);
      _set(Conn.disconnected,
          'HID Trap "${kDuckyScripts[idx]}" armed — advertising as a keyboard. '
          'BLE dropped by the handoff; press the C3 BOOT button, then Reconnect.');
    } catch (e) {
      _snack('Write failed: $e');
    }
  }

  Future<void> _stop() async {
    if (_cmd == null) return;
    try { await _cmd!.write([kOpStop], withoutResponse: false); _snack('STOP → IDLE'); }
    catch (e) { _snack('Stop failed: $e'); }
  }

  Future<void> _reconnect() async {
    _cmd = null; _stat = null; _data = null; _dataBuf = '';
    _startScan();
  }

  void _snack(String s) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(s), duration: const Duration(seconds: 2)));
  }

  @override
  Widget build(BuildContext context) {
    final ready = _conn == Conn.ready;
    return Scaffold(
      appBar: AppBar(
        title: const Text('CyberDeck'),
        actions: [
          if (_conn == Conn.scanning)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)),
            ),
        ],
      ),
      body: Column(
        children: [
          _statusCard(ready),
          Expanded(
            child: ready
                ? _modeGrid()
                : Center(child: _connectCta()),
          ),
        ],
      ),
    );
  }

  Widget _statusCard(bool ready) {
    Color dot;
    switch (_conn) {
      case Conn.ready: dot = Colors.greenAccent; break;
      case Conn.scanning:
      case Conn.connecting:
      case Conn.bonding: dot = Colors.amberAccent; break;
      default: dot = Colors.redAccent;
    }
    return Card(
      margin: const EdgeInsets.all(12),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Icon(Icons.circle, color: dot, size: 14),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(_status, style: const TextStyle(fontWeight: FontWeight.w600)),
                  if (ready && _deckStatus.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 4),
                      child: Text(_deckStatus,
                          style: const TextStyle(fontFamily: 'monospace', fontSize: 13)),
                    ),
                ],
              ),
            ),
            if (ready)
              FilledButton.tonal(onPressed: _stop, child: const Text('STOP')),
          ],
        ),
      ),
    );
  }

  Widget _connectCta() {
    final busy = _conn == Conn.scanning || _conn == Conn.connecting || _conn == Conn.bonding;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(_status, textAlign: TextAlign.center),
        const SizedBox(height: 16),
        FilledButton.icon(
          onPressed: busy ? null : _reconnect,
          icon: const Icon(Icons.bluetooth_searching),
          label: Text(_conn == Conn.disconnected ? 'Reconnect' : 'Scan'),
        ),
      ],
    );
  }

  Widget _modeGrid() {
    return ListView(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 24),
      children: [
        ValueListenableBuilder<ApTarget?>(
          valueListenable: _target,
          builder: (_, tgt, __) => Card(
            margin: const EdgeInsets.only(bottom: 4),
            color: (tgt == null ? Colors.orange : Colors.green).withOpacity(0.10),
            child: ListTile(
              dense: true,
              leading: Icon(tgt == null ? Icons.gps_off : Icons.gps_fixed,
                  color: tgt == null ? Colors.orangeAccent : Colors.greenAccent),
              title: Text(tgt == null ? 'No attack target set' : 'Target: ${tgt.ssid.isEmpty ? tgt.bssid : tgt.ssid}',
                  style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600)),
              subtitle: Text(tgt == null ? 'Active attacks that need one are blocked' : '${tgt.bssid} · ch ${tgt.ch}',
                  style: const TextStyle(fontSize: 11)),
              trailing: const Icon(Icons.chevron_right),
              onTap: _openTargetPage,
            ),
          ),
        ),
        const SizedBox(height: 8),
        _sectionLabel('WiFi  ·  drops BLE while running'),
        _grid(kWifiModes),
        const SizedBox(height: 16),
        _sectionLabel('Bluetooth  ·  stays connected'),
        _grid(kBleModes),
      ],
    );
  }

  Widget _sectionLabel(String s) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Text(s, style: TextStyle(color: Colors.grey[400], fontSize: 12, letterSpacing: 0.5)),
      );

  Widget _grid(List<DeckMode> modes) {
    return GridView.count(
      crossAxisCount: 2,
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      childAspectRatio: 2.4,
      mainAxisSpacing: 10,
      crossAxisSpacing: 10,
      children: modes.map((m) {
        return OutlinedButton(
          style: OutlinedButton.styleFrom(
            side: BorderSide(color: m.isTx ? Colors.redAccent : Colors.tealAccent.withOpacity(0.5)),
          ),
          onPressed: () => _openDetail(m),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(m.name, textAlign: TextAlign.center, style: const TextStyle(fontSize: 13)),
              if (m.isTx)
                const Text('TX', style: TextStyle(fontSize: 10, color: Colors.redAccent)),
            ],
          ),
        );
      }).toList(),
    );
  }
}

// Full-screen briefing for one mode: what it does, how it works, and a Run button.
class ModeDetailPage extends StatefulWidget {
  final DeckMode mode;
  final void Function(int scriptIdx) onRun;
  final ValueListenable<ApTarget?>? target;   // current locked target (for targetable modes)
  final VoidCallback? onChooseTarget;         // opens the target picker
  const ModeDetailPage({super.key, required this.mode, required this.onRun, this.target, this.onChooseTarget});
  @override
  State<ModeDetailPage> createState() => _ModeDetailPageState();
}

class _ModeDetailPageState extends State<ModeDetailPage> {
  int _scriptIdx = 0;

  @override
  Widget build(BuildContext context) {
    final m = widget.mode;
    final info = kModeInfo[m.opcode];
    final isHid = m.opcode == kOpHidTrap;
    final accent = m.isTx ? Colors.redAccent : Colors.tealAccent;

    return Scaffold(
      appBar: AppBar(title: Text(m.name)),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 120),
        children: [
          // badges
          Wrap(spacing: 8, runSpacing: 8, children: [
            _chip(m.isTx ? 'ACTIVE · TRANSMITS' : 'PASSIVE · LISTEN-ONLY',
                m.isTx ? Colors.redAccent : Colors.greenAccent),
            _chip(m.dropsBle ? 'Drops BLE link' : 'Phone stays connected',
                m.dropsBle ? Colors.orangeAccent : Colors.tealAccent),
          ]),
          const SizedBox(height: 20),
          if (info != null) ...[
            _heading('What it does', accent),
            Text(info.what, style: const TextStyle(fontSize: 15, height: 1.4)),
            const SizedBox(height: 20),
            _heading('How it works', accent),
            Text(info.how, style: const TextStyle(fontSize: 15, height: 1.4, color: Colors.white70)),
          ],
          if (m.isTx) ...[
            const SizedBox(height: 20),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.red.withOpacity(0.12),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.redAccent.withOpacity(0.4)),
              ),
              child: const Row(children: [
                Icon(Icons.warning_amber, color: Colors.redAccent, size: 20),
                SizedBox(width: 10),
                Expanded(child: Text(
                  'Active transmission. Use only against hardware you own or have written permission to test.',
                  style: TextStyle(fontSize: 13))),
              ]),
            ),
          ],
          if (m.targetable && widget.target != null) ...[
            const SizedBox(height: 24),
            _heading('Target', accent),
            ValueListenableBuilder<ApTarget?>(
              valueListenable: widget.target!,
              builder: (_, tgt, __) {
                final ok = tgt != null;
                final c = ok ? Colors.greenAccent : Colors.orangeAccent;
                return Container(
                  padding: const EdgeInsets.all(14),
                  decoration: BoxDecoration(
                    color: c.withOpacity(0.10),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: c.withOpacity(0.4)),
                  ),
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Row(children: [
                      Icon(ok ? Icons.gps_fixed : Icons.gps_off, color: c, size: 20),
                      const SizedBox(width: 10),
                      Expanded(child: Text(
                        ok
                            ? '${tgt.ssid.isEmpty ? "(hidden network)" : tgt.ssid}\n${tgt.bssid}  ·  ch ${tgt.ch}'
                            : (m.requiresTarget
                                ? 'No target set — required before this can run.'
                                : 'No target set (optional — scopes this mode to one network).'),
                        style: const TextStyle(fontSize: 14, height: 1.3))),
                    ]),
                    const SizedBox(height: 10),
                    OutlinedButton.icon(
                      onPressed: widget.onChooseTarget,
                      icon: const Icon(Icons.wifi_find, size: 18),
                      label: Text(ok ? 'Change target' : 'Choose target'),
                    ),
                  ]),
                );
              },
            ),
          ],
          if (m.dropsBle && !isHid) ...[
            const SizedBox(height: 16),
            Text('Running this hands the radio to WiFi, so the phone link drops. '
                'Press the C3 BOOT button (or power-cycle) to return to IDLE, then Reconnect.',
                style: TextStyle(fontSize: 13, color: Colors.grey[400])),
          ],
          if (isHid) ...[
            const SizedBox(height: 24),
            _heading('Script to type', accent),
            const SizedBox(height: 4),
            DropdownButtonFormField<int>(
              value: _scriptIdx,
              isExpanded: true,
              decoration: const InputDecoration(border: OutlineInputBorder()),
              items: [
                for (int i = 0; i < kDuckyScripts.length; i++)
                  DropdownMenuItem(value: i, child: Text(kDuckyScripts[i])),
              ],
              onChanged: (v) => setState(() => _scriptIdx = v ?? 0),
            ),
          ],
        ],
      ),
      bottomSheet: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: (m.requiresTarget && widget.target != null)
              ? ValueListenableBuilder<ApTarget?>(
                  valueListenable: widget.target!,
                  builder: (_, tgt, __) =>
                      _runBtn(m, isHid, enabled: tgt != null, disabledLabel: 'Set a target to run'),
                )
              : _runBtn(m, isHid, enabled: true),
        ),
      ),
    );
  }

  Widget _runBtn(DeckMode m, bool isHid, {required bool enabled, String? disabledLabel}) {
    return SizedBox(
      width: double.infinity,
      height: 52,
      child: FilledButton.icon(
        style: FilledButton.styleFrom(backgroundColor: m.isTx ? Colors.redAccent : null),
        icon: Icon(!enabled ? Icons.block : (m.isTx ? Icons.bolt : Icons.play_arrow)),
        label: Text(!enabled
            ? (disabledLabel ?? 'Unavailable')
            : (isHid ? 'Arm trap: ${kDuckyScripts[_scriptIdx]}' : 'Run ${m.name}')),
        onPressed: enabled
            ? () { widget.onRun(_scriptIdx); Navigator.pop(context); }
            : null,
      ),
    );
  }

  Widget _chip(String s, Color c) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        decoration: BoxDecoration(
          color: c.withOpacity(0.14),
          borderRadius: BorderRadius.circular(20),
          border: Border.all(color: c.withOpacity(0.5)),
        ),
        child: Text(s, style: TextStyle(fontSize: 12, color: c, fontWeight: FontWeight.w600)),
      );

  Widget _heading(String s, Color c) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Text(s.toUpperCase(),
            style: TextStyle(fontSize: 13, letterSpacing: 1, color: c, fontWeight: FontWeight.w700)),
      );
}

// Target picker: scan-and-select from the C3's network list, or type a BSSID in by hand.
class TargetPage extends StatefulWidget {
  final ValueListenable<List<ApTarget>> aps;
  final ValueListenable<ApTarget?> target;
  final ValueListenable<bool> busy;
  final Future<void> Function() onScan;
  final Future<void> Function(ApTarget) onPick;
  final Future<void> Function() onClear;
  const TargetPage({
    super.key,
    required this.aps,
    required this.target,
    required this.busy,
    required this.onScan,
    required this.onPick,
    required this.onClear,
  });
  @override
  State<TargetPage> createState() => _TargetPageState();
}

class _TargetPageState extends State<TargetPage> {
  final _bssidC = TextEditingController();
  final _ssidC = TextEditingController();
  final _chC = TextEditingController();

  @override
  void dispose() {
    _bssidC.dispose(); _ssidC.dispose(); _chC.dispose();
    super.dispose();
  }

  Color _rssiColor(int r) =>
      r > -60 ? Colors.greenAccent : (r > -75 ? Colors.amberAccent : Colors.redAccent);

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Choose target')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          ValueListenableBuilder<ApTarget?>(
            valueListenable: widget.target,
            builder: (_, tgt, __) => tgt == null
                ? const SizedBox.shrink()
                : Card(
                    color: Colors.green.withOpacity(0.12),
                    child: ListTile(
                      leading: const Icon(Icons.gps_fixed, color: Colors.greenAccent),
                      title: Text(tgt.ssid.isEmpty ? '(hidden network)' : tgt.ssid),
                      subtitle: Text('${tgt.bssid} · ch ${tgt.ch}'),
                      trailing: TextButton(onPressed: () => widget.onClear(), child: const Text('Clear')),
                    ),
                  ),
          ),
          const SizedBox(height: 8),
          ValueListenableBuilder<bool>(
            valueListenable: widget.busy,
            builder: (_, busy, __) => SizedBox(
              height: 48,
              child: FilledButton.icon(
                onPressed: busy ? null : () => widget.onScan(),
                icon: busy
                    ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.radar),
                label: Text(busy ? 'Scanning…' : 'Scan for networks (~8s)'),
              ),
            ),
          ),
          const SizedBox(height: 6),
          Text('The deck drops BLE for ~8s to scan, then reconnects on its own and lists what it found.',
              style: TextStyle(fontSize: 12, color: Colors.grey[500])),
          const SizedBox(height: 18),
          const Text('Found networks', style: TextStyle(fontWeight: FontWeight.w600)),
          ValueListenableBuilder<List<ApTarget>>(
            valueListenable: widget.aps,
            builder: (_, aps, __) {
              if (aps.isEmpty) {
                return const Padding(
                  padding: EdgeInsets.symmetric(vertical: 16),
                  child: Text('No networks yet — run a scan above.', style: TextStyle(color: Colors.grey)));
              }
              return Column(children: [
                for (final ap in aps)
                  ListTile(
                    contentPadding: EdgeInsets.zero,
                    leading: Icon(Icons.wifi, color: _rssiColor(ap.rssi)),
                    title: Text(ap.ssid.isEmpty ? '(hidden network)' : ap.ssid),
                    subtitle: Text('${ap.bssid} · ch ${ap.ch} · ${ap.rssi} dBm'),
                    trailing: const Icon(Icons.chevron_right),
                    onTap: () async {
                      await widget.onPick(ap);
                      if (mounted) Navigator.pop(context);
                    },
                  ),
              ]);
            },
          ),
          const Divider(height: 32),
          const Text('Or type a BSSID', style: TextStyle(fontWeight: FontWeight.w600)),
          const SizedBox(height: 10),
          TextField(
            controller: _bssidC,
            autocorrect: false,
            decoration: const InputDecoration(
                labelText: 'BSSID  (AA:BB:CC:DD:EE:FF)', border: OutlineInputBorder()),
          ),
          const SizedBox(height: 10),
          Row(children: [
            Expanded(
              flex: 2,
              child: TextField(
                controller: _ssidC,
                decoration: const InputDecoration(labelText: 'SSID (optional)', border: OutlineInputBorder()),
              ),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: TextField(
                controller: _chC,
                keyboardType: TextInputType.number,
                decoration: const InputDecoration(labelText: 'Ch', border: OutlineInputBorder()),
              ),
            ),
          ]),
          const SizedBox(height: 14),
          SizedBox(
            height: 48,
            child: FilledButton.icon(
              onPressed: () async {
                final b = _bssidC.text.trim();
                if (b.isEmpty) return;
                final ap = ApTarget(b, int.tryParse(_chC.text.trim()) ?? 0, 0, _ssidC.text.trim());
                await widget.onPick(ap);
                if (mounted) Navigator.pop(context);
              },
              icon: const Icon(Icons.gps_fixed),
              label: const Text('Set as target'),
            ),
          ),
        ],
      ),
    );
  }
}
