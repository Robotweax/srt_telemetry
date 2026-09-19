# Folgeauftrag: Linux-Lab-Ergebnisse präzisieren und mit netem nachtesten

Ausgangspunkt ist Network-Lab-PR #48 und dessen Evidenzarchiv vom 19.09.2026.
Die SDK-Nachprüfung ist in `testdata/linux-lab-2026-09-19/README.md` dokumentiert.

Stand nach der Untersuchung: Die Sender-Drop-Semantik wurde durch SRT-PR #48
(Merge `09316eda48e3360808fff2021eddc2f2bf821b06`) geklärt: lokale
TTL-/TLPKTDROP-Pufferentfernungen beweisen keinen Verlust der Anwendungspayload.
Die paketbezogene Evidenz liegt in Network-Lab-PR #50. Dieser Folgeauftrag
bleibt als historischer Untersuchungsplan erhalten; die Semantikfrage ist
für das untersuchte Profil beantwortet. Siehe die
[aktuelle Metrikdefinition](../architecture/srt-stats-field-map.md#sender-drop-interpretation).

## Zuerst die Ergebniszuordnung korrigieren

Analysiere die sechs abgewiesenen JSONL-Paare mit dem Analyzer der qualifizierten
Revision `9fe30a833c0c5f755a5f57249103f1a955790b80`. Bewahre die neuen
Analyzer-Ausgaben separat von den ursprünglichen Aufzeichnungen auf.

Erwarteter Replay-Befund: drei Empfängerpausen mit Haivision-Sender ergeben
`insufficient-evidence`; drei Bandbreitenreduktionen mit Robotweax-Sender ergeben
`sender-side-pressure` mit `supported`. Die Capture-Runner weisen alle sechs
Läufe bereits vor der Analyse ab. Erfasse deshalb Capture-Gate und
Analyzer-Ergebnis in getrennten Feldern und prüfe sie im Validator.
Die Qualifikationsbilanz bleibt 24/30. Bewahre die ursprüngliche Zusammenfassung
als Historie; erkläre die Korrektur in der neuen Fassung.

## Danach kontrollierte netem-Nachtests

Nutze die vorhandene Lab-Topologie und die freigegebenen Testinterfaces.
Die aktive Störung muss durch Kernel-qdisc/netem beziehungsweise den vorhandenen
Kernel-Bandbreitenbegrenzer entstehen. UDP-Proxys dürfen dabei keine zusätzliche
Störung injizieren. Falls die Runner diese Topologie noch nicht unterstützen,
dokumentiere und implementiere zuerst einen expliziten externen Störungsmodus.
Eine Empfängerpause wird weiterhin in der Anwendung ausgelöst.

Halte Provider-Revisionen, Buildprofile, Last, Payload, SRT-Latenzen und tatsächlich
wirksame UDP-Puffer fest. Prüfe die Puffer vor dem Start. Erfasse qdisc-Konfiguration
und Zähler vor, während und nach jedem Lauf sowie die Wiederherstellung.

Beginne mit Baselines in beiden Richtungen. Wiederhole danach Empfängerpause und
Bandbreitenreduktion je dreimal pro Richtung. Nutze zunächst dieselben Parameter
wie im ursprünglichen Lauf und ändere in Folgeexperimenten jeweils nur einen
Parameter. Erfasse für jede Wiederholung Rohdaten, Capture-Gate, Analyzer-Ausgabe,
Anwendungs-Sende-/Empfangszähler und Payload-Integrität, auch bei Gate-Ablehnung.

Untersuche gezielt:

- Empfängerpause: Pausendauer und Empfängerpufferverlauf; ob und wann beim
  Haivision-Sender Warteschlange oder Flight Size ansteigen. Erfasse die
  tatsächlichen Anwendungspausen unabhängig von der Telemetrie.
- Bandbreitenreduktion: Ursprung der Robotweax-Sender-Drops bei vollständigen
  2.400/2.400 Unique-Paketzählern. Vergleiche kumulative Zähler, Deltas,
  Drain-/ACK-Verlauf, konfigurierte Latenz und Anwendungs-Payload-Integrität.
  Bezeichne die Drops erst nach Klärung als verlorene Anwendungsdaten.
- Trenne die Beobachtung „Senderdruck“ von der engeren Szenarioanforderung
  „reines Warteschlangenwachstum ohne Drops“.

Verwende monotone lokale Zeitstempel für lokale Abläufe. Hostübergreifende
Reihenfolgen benötigen dokumentierte Uhrensynchronisation samt Unsicherheit.
Verändere Diagnosekriterien nicht nur, damit die Testmatrix grün wird.
Falls ein Protokollproblem belegt wird, erstelle einen reproduzierbaren Auftrag
für den Robotweax-SRT-Protokoll-Agenten.

Übergabe: neue Ergebnisübersicht, genaue Befehle, Revisionen, vollständige
Rohaufzeichnungen und Checksummen sowie verbleibende Grenzen. Diese technischen
Tests erzeugen keine menschlichen Operator-Messwerte.
