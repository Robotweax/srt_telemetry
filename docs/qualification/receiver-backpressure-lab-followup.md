# Lab-Auftrag: Empfängerpuffer und bestätigten Rückstau getrennt prüfen

## Ziel und Ausgangspunkt

Die netem-Läufe aus Network-Lab-PR #48 zeigen bei Empfängerpausen stabil
9.212.000 Byte Pufferbelegung. Ein abgeschlossener Burst von 7.000 Nachrichten
passt in den Empfangspuffer; eine längere Lesepause erzeugt deshalb nicht
zwangsläufig Rückstau zum Sender. Der SDK-Bericht zeigt diese Beobachtung jetzt
unabhängig von der engeren Ursachendiagnose. Deren Schwellen bleiben unverändert.

Nutze die freigegebene disposable Linux-Lab-VM und die vorhandene direkte
Namespace-/veth-Topologie. Halte den Netzwerkpfad für diese Untersuchung
unbeeinträchtigt. Übernimm die bestehenden Cleanup- und UDP-Pufferkontrollen.

## Durchführung

1. Dokumentiere exakte Provider-/SDK-Revisionen, Binäridentitäten, effektive
   SRT- und Kernel-Puffer, Latenz, Payloadgröße und Senderate. Falls der Runner
   die folgenden Varianten noch nicht unterstützt, ergänze explizite Optionen
   samt Tests. Verwende die tatsächlich unterstützten Optionen aus `--help`.
2. Wiederhole die bestehende Baseline und das unveränderte Pauseprofil in
   beiden Provider-Richtungen als Kontrollen. Plane drei Wiederholungen je
   Profil und Richtung; sichere auch abgebrochene Läufe.
3. Variante A: Ändere ausschließlich den SRT-Empfangspuffer. Setze ihn über die
   öffentliche API vor Verbindungsaufbau so, dass der bisherige Burst nicht
   vollständig hineinpasst; dokumentiere den ausgehandelten/effektiven Wert.
   Verändere nicht gleichzeitig den Kernel-UDP-Puffer oder Diagnoseschwellen.
4. Variante B: Kehre zum ursprünglichen SRT-Puffer zurück und ersetze den
   einmaligen Burst durch fortlaufendes, begrenztes Senden während der
   Lesepause. Dokumentiere die angebotene Rate und erfolgreich akzeptierte
   Datenmenge. Sender und Empfänger müssen unabhängig steuerbar bleiben,
   damit ein blockierender Send-Aufruf das Ende der Lesepause nicht verhindert.
5. Nutze für beide Varianten vorab dokumentierte Laufzeit-, Datenmengen- und
   Speichergrenzen. Setze die Empfänger-Reads nach der geplanten Pause wieder
   fort, auch wenn der Sender blockiert. Beende und kennzeichne einen Lauf,
   falls keine kontrollierte Erholung innerhalb der festgelegten Frist erfolgt.

## Evidenz und Auswertung

Erfasse lokal monotone Zeitpunkte für Pause/Resume, tatsächliche Send-/Read-
Fortschritte, Send-Blockaden beziehungsweise EASYNCSND, Empfängerbelegung,
Senderpuffer und Flight Size. Ergänze nach Verfügbarkeit das öffentliche
Flow-Window, qdisc-Zähler, alle Drop-/Loss-/Retransmissionszähler sowie
Anwendungsnachrichten-Zähler und Payload-Integrität. Bewahre auch den
Erholungsverlauf auf. Gleich große Paketzähler allein ersetzen keine
Payloadprüfung; Sender-Drops belegen keinen Payloadverlust.

Trenne in jeder Ergebniszeile:

- Beobachtung: Empfängerbelegung überschreitet den bestehenden Schwellenwert.
- Senderbestätigung: überlappende Puffer-/Flight-Zunahme vorhanden, unterhalb
  der Schwellen oder wegen fehlender Werte nicht beurteilbar.
- Capture-Gate: bestanden/abgewiesen mit Grund.
- Analyzer: tatsächliche Hypothese und Assessment, auch nach Gate-Ablehnung.
- Anwendungszustellung und Erholung: separat gemessen, mit allen Abweichungen.

Ein ausbleibender Senderanstieg ist ein dokumentiertes Ergebnis. Passe weder
Schwellen noch Referenzantworten an, um eine Diagnose zu erzwingen. Falls ein
anderes Signal Rückstau klarer beschreibt, liefere seine Messwerte und einen
konkreten Vorschlag zur getrennten SDK-Bewertung.

Übergabe: Matrix, Roh-JSONL/CSV, Analyzer-Ausgaben, genaue Befehle, neue
Runner-Optionen, Revisionen, Checksummen und Cleanup-Nachweise. Keine
menschlichen Messwerte erzeugen; diese Tests dienen der technischen Evidenz.
