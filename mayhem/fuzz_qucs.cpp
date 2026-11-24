// libFuzzer harness for the Qucs schematic tool's headless CLI operations.
//
// The `qucs` binary's batch mode (`qucs -n|-p|-d -i in -o out`, main.cpp) calls doNetlist / doPrint /
// doDump from qucs_functions.h on the input file. This harness does the same, in process, on a file
// holding the fuzz input. FUZZ_MODE selects the operation (one binary per mode, built by build.sh):
//
//   FUZZ_NETLIST  data = schematic (.sch legacy format, or anything else -> Verilog-schematic reader),
//                 doNetlist(in, out.net)                                   (= qucs -n)
//   FUZZ_PRINT    data = schematic, doPrint(in, out.pdf, ...)              (= qucs -p -o x.pdf)
//   FUZZ_DUMP     data = schematic, doDump(in, out.sch|out.vs)             (= qucs -d [-t sch|vs])
//   FUZZ_LIB      data = component library file fuzzlib.lib that a fixed schematic's <Lib> component
//                 references; QucsSettings.LibDir points at the scratch dir, doNetlist(fixed.sch)
//   FUZZ_DAT      data = simulation dataset dat.dat beside a fixed schematic dat.sch whose diagrams
//                 (Rect, Smith, Tab; no Polar: PolarDiagram's
//                 grid code converts NaN to int on any schematic without data, which would stop every
//                 rejected dataset there) plot variables from it, doPrint(dat.sch, out.pdf)
//   FUZZ_SUB      data = sub-circuit schematic sub.sch that a fixed parent's <Sub> component uses,
//                 doNetlist(parent.sch)
//   FUZZ_HDL      data = HDL source, written as both fuzz.vhdl and fuzz.v, which a fixed schematic's
//                 <VHDL> and <Verilog> file components reference (their entity/module header scanners
//                 build the component symbols on load), doNetlist(hdl.sch)
//   FUZZ_VA       data = Verilog-A source fuzz.va, the symbol source (DataDisplay) of a fixed schematic
//                 va.sch. Qucs has no Verilog-A file component: the only place the Verilog-A header
//                 scanner (components/vafile.cpp, VerilogA_File_Info) reads a file is
//                 Schematic::adjustPortNumbers(), which builds the symbol's ports from the .va module
//                 header whenever the schematic is saved (saveAs) or its symbol is edited. That code
//                 calls App->findDoc(), so it needs the QucsApp the GUI passes in (openSchematic()
//                 passes none, so the batch -d path would crash on the null App before it reads the
//                 file). The harness does what the GUI does on open + save: Schematic(QucsMain, va.sch),
//                 loadDocument(), saveAs(out.sch).
//
// State kept across inputs, on purpose:
//  * the QApplication and the QucsSettings initialised once, as main.cpp does (read-only afterwards,
//    except LibDir which FUZZ_LIB sets to the same scratch dir every time);
//  * the component module registry: openSchematic() calls Module::registerModules() on every call,
//    which appends every module to its category again. resetModules() empties the registry after each
//    input so every input starts from the same state. (Upstream's Module::unregisterModules() cannot be
//    used: it deletes each module twice, via its Category and again via Module::Modules.)
//  * FUZZ_DUMP and FUZZ_VA only: one QucsApp main window, as the CLI creates before doDump (Schematic::saveAs
//    calls QucsMain->updateSchNameHash() unconditionally, so doDump needs it). Its open documents are
//    not touched by doDump, which works on its own Schematic.
//  * a scratch directory under $TMPDIR, whose files are rewritten for every input.
//
// Upstream code pops modal QMessageBoxes on some errors (e.g. future document versions); under the
// offscreen platform a modal dialog would wait forever for a click. An event filter rejects every
// dialog as soon as it is shown, which is what the batch CLI user would see as "Cancel/OK".
#include "qucs_functions.h"

#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QMetaObject>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifndef FUZZ_MODE
#error "define FUZZ_MODE"
#endif
#define FUZZ_NETLIST 1
#define FUZZ_PRINT 2
#define FUZZ_DUMP 3
#define FUZZ_LIB 4
#define FUZZ_DAT 5
#define FUZZ_SUB 6
#define FUZZ_HDL 7
#define FUZZ_VA 8

namespace {

class DialogCloser : public QObject {
 protected:
  bool eventFilter(QObject *obj, QEvent *ev) override {
    if (ev->type() == QEvent::Show) {
      if (QDialog *dlg = qobject_cast<QDialog *>(obj))
        QMetaObject::invokeMethod(dlg, "reject", Qt::QueuedConnection);
    }
    return false;
  }
};

void quietHandler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
  // Debug/warning output is noise for the fuzzer. A fatal message is printed and the handler returns:
  // Qt itself then aborts in qt_message_output() (qt_message_fatal), so the crash stack ends in Qt
  // and the application frame that asserted, not in this handler.
  if (type == QtFatalMsg) fprintf(stderr, "qFatal: %s\n", msg.toLocal8Bit().constData());
}

QApplication *app = nullptr;
QTemporaryDir *scratch = nullptr;
QString inPath, outPath, libSchPath;

const char kLibSchematic[] =
    "<Qucs Schematic 0.0.20>\n"
    "<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n"
    "  <Vdc V1 1 140 250 18 -26 0 1 \"1 V\" 1>\n"
    "  <GND * 5 140 320 0 0 0 0>\n"
    "  <R R1 1 340 250 -22 15 1 3 \"50 Ohm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
    "  <Lib L1 1 250 200 0 0 0 0 \"fuzzlib\" 0 \"Comp\" 0>\n"
    "  <.DC DC1 1 140 380 0 43 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
    "</Components>\n"
    "<Wires>\n"
    "<140 280 140 300 \"\" 0 0 0 \"\">\n<140 300 340 300 \"\" 0 0 0 \"\">\n"
    "<340 280 340 300 \"\" 0 0 0 \"\">\n<140 200 140 220 \"\" 0 0 0 \"\">\n"
    "<140 200 220 200 \"\" 0 0 0 \"\">\n<340 200 340 220 \"\" 0 0 0 \"\">\n"
    "<280 200 340 200 \"\" 0 0 0 \"\">\n"
    "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

void resetModules() {
  // Each Category owns (and deletes) its modules; Module::Modules only indexes them.
  while (!Category::Categories.isEmpty()) delete Category::Categories.takeFirst();
  Module::Modules.clear();
}

const char kDatSchematic[] =
    "<Qucs Schematic 0.0.20>\n"
    "<Properties>\n  <DataSet=dat.dat>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n  <GND * 5 140 320 0 0 0 0>\n</Components>\n<Wires>\n</Wires>\n"
    "<Diagrams>\n"
    "  <Rect 320 661 334 211 3 #c0c0c0 1 10 1 10 1 1e+07 1 -31.9617 20 20 1 -215.509 100 215.373 315 0 225 \"\" \"\" \"\">\n"
    "\t<\"v1\" #0000ff 0 3 0 0 0>\n\t<\"v2\" #ff0000 0 3 0 0 1>\n"
    "  </Rect>\n"
    "  <Smith 751 599 219 219 3 #c0c0c0 1 00 1 0 1 1 1 0 4 1 1 0 1 1 315 0 225 \"\" \"\" \"\">\n"
    "\t<\"S[1,1]\" #0000ff 2 3 0 1 0>\n"
    "  </Smith>\n"
    "  <Tab 360 868 164 49 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 \"\" \"\" \"\" \"\">\n"
    "\t<\"v1\" #0000ff 0 3 0 0 0>\n\t<\"v2\" #0000ff 0 3 0 0 0>\n"
    "  </Tab>\n"
    "</Diagrams>\n<Paintings>\n</Paintings>\n";

const char kSubParent[] =
    "<Qucs Schematic 0.0.20>\n"
    "<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n"
    "  <Vdc V1 1 140 250 18 -26 0 1 \"1 V\" 1>\n"
    "  <GND * 5 140 320 0 0 0 0>\n"
    "  <Sub SUB1 1 250 200 0 0 0 0 \"sub.sch\" 0>\n"
    "  <.DC DC1 1 140 380 0 43 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
    "</Components>\n"
    "<Wires>\n<140 200 140 220 \"\" 0 0 0 \"\">\n<140 200 220 200 \"\" 0 0 0 \"\">\n"
    "<140 280 140 320 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

const char kHdlSchematic[] =
    "<Qucs Schematic 0.0.20>\n"
    "<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n"
    "  <VHDL X1 1 200 200 0 0 0 0 \"fuzz.vhdl\" 0>\n"
    "  <Verilog X2 1 400 200 0 0 0 0 \"fuzz.v\" 0>\n"
    "  <GND * 5 140 320 0 0 0 0>\n"
    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

QString hdlCopy;

const char kVaSchematic[] =
    "<Qucs Schematic 0.0.20>\n"
    "<Properties>\n  <View=0,0,800,800,1,0,0>\n  <DataDisplay=fuzz.va>\n</Properties>\n"
    "<Symbol>\n  <.ID -20 14 X>\n  <.PortSym 40 20 1 0>\n</Symbol>\n"
    "<Components>\n  <GND * 5 140 320 0 0 0 0>\n</Components>\n"
    "<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

bool writeFile(const QString &path, const uint8_t *data, size_t size) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  bool ok = f.write(reinterpret_cast<const char *>(data), (qint64)size) == (qint64)size;
  f.close();
  return ok;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) {
  setenv("QT_QPA_PLATFORM", "offscreen", 0);
  qInstallMessageHandler(quietHandler);

  // Same defaults main.cpp applies before the batch operations.
  QucsVersion = VersionTriplet(PACKAGE_VERSION);
  QucsSettings.font = QFont("Helvetica", 12);
  QucsSettings.largeFontSize = 16.0;
  QucsSettings.maxUndo = 20;
  QucsSettings.NodeWiring = 0;
  QucsSettings.Editor = "qucs";
  QucsSettings.IgnoreFutureVersion = false;

  static int qargc = 1;
  static char *qargv[] = {(*argv)[0], nullptr};
  app = new QApplication(qargc, qargv);
  app->installEventFilter(new DialogCloser());
  QucsSettings.x = 100; QucsSettings.y = 100; QucsSettings.dx = 600; QucsSettings.dy = 400;
  QucsSettings.BGColor.setRgb(255, 250, 225);
  app->setFont(QucsSettings.font);
  QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
  setlocale(LC_NUMERIC, "C");

  const char *tmp = getenv("TMPDIR");
  scratch = new QTemporaryDir(QString(tmp && *tmp ? tmp : "/tmp") + "/qucsfuzz-XXXXXX");
  if (!scratch->isValid()) abort();
  atexit([] { delete scratch; });  // removes the scratch dir
  QDir d(scratch->path());
  QucsSettings.QucsHomeDir.setPath(d.absoluteFilePath("home"));
  QucsSettings.QucsWorkDir.setPath(scratch->path());
  QucsSettings.BinDir = d.absolutePath() + "/bin/";
  QucsSettings.Qucsator = QucsSettings.BinDir + "qucsator";
  QucsSettings.Qucsconv = QucsSettings.BinDir + "qucsconv";

#if FUZZ_MODE == FUZZ_LIB
  QucsSettings.LibDir = d.absolutePath() + "/";
  inPath = d.absoluteFilePath("fuzzlib.lib");
  libSchPath = d.absoluteFilePath("libtest.sch");
  if (!writeFile(libSchPath, reinterpret_cast<const uint8_t *>(kLibSchematic), strlen(kLibSchematic)))
    abort();
  outPath = d.absoluteFilePath("out.net");
#elif FUZZ_MODE == FUZZ_HDL
  QucsSettings.LibDir = QString("/mayhem/qucs-lib/library/");
  inPath = d.absoluteFilePath("fuzz.vhdl");
  hdlCopy = d.absoluteFilePath("fuzz.v");
  libSchPath = d.absoluteFilePath("hdl.sch");
  outPath = d.absoluteFilePath("out.net");
  if (!writeFile(libSchPath, reinterpret_cast<const uint8_t *>(kHdlSchematic), strlen(kHdlSchematic)))
    abort();
#elif FUZZ_MODE == FUZZ_VA
  QucsSettings.LibDir = QString("/mayhem/qucs-lib/library/");
  inPath = d.absoluteFilePath("fuzz.va");
  libSchPath = d.absoluteFilePath("va.sch");
  outPath = d.absoluteFilePath("out.sch");
  if (!writeFile(libSchPath, reinterpret_cast<const uint8_t *>(kVaSchematic), strlen(kVaSchematic)))
    abort();
  QucsMain = new QucsApp(QStringList());
#elif FUZZ_MODE == FUZZ_DAT || FUZZ_MODE == FUZZ_SUB
  QucsSettings.LibDir = QString("/mayhem/qucs-lib/library/");
#if FUZZ_MODE == FUZZ_DAT
  inPath = d.absoluteFilePath("dat.dat");
  libSchPath = d.absoluteFilePath("dat.sch");
  const char *fixed = kDatSchematic;
  outPath = d.absoluteFilePath("out.pdf");
#else
  inPath = d.absoluteFilePath("sub.sch");
  libSchPath = d.absoluteFilePath("parent.sch");
  const char *fixed = kSubParent;
  outPath = d.absoluteFilePath("out.net");
#endif
  if (!writeFile(libSchPath, reinterpret_cast<const uint8_t *>(fixed), strlen(fixed))) abort();
#else
  const char *lib = getenv("QUCS_LIBRARY");
  QucsSettings.LibDir = lib ? QString(lib) : QString("/mayhem/qucs-lib/library/");
  inPath = d.absoluteFilePath("input.sch");
#if FUZZ_MODE == FUZZ_PRINT
  outPath = d.absoluteFilePath("out.pdf");
#elif FUZZ_MODE == FUZZ_DUMP
  outPath = d.absoluteFilePath("out");
  QucsMain = new QucsApp(QStringList());
#else
  outPath = d.absoluteFilePath("out.net");
#endif
#endif
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!writeFile(inPath, data, size)) abort();

#if FUZZ_MODE == FUZZ_NETLIST
  doNetlist(inPath, outPath);
#elif FUZZ_MODE == FUZZ_LIB || FUZZ_MODE == FUZZ_SUB
  doNetlist(libSchPath, outPath);
#elif FUZZ_MODE == FUZZ_HDL
  if (!writeFile(hdlCopy, data, size)) abort();
  doNetlist(libSchPath, outPath);
#elif FUZZ_MODE == FUZZ_VA
  // = openSchematic(), but with the GUI's QucsApp, then the GUI's save.
  Module::registerModules();
  {
    Schematic *sch = new Schematic(QucsMain, libSchPath);
    if (sch->loadDocument()) sch->saveAs(outPath, "sch");
    delete sch;
  }
#elif FUZZ_MODE == FUZZ_DAT
  doPrint(libSchPath, outPath, "A4", 96, "RGB", "portrait");
#elif FUZZ_MODE == FUZZ_PRINT
  doPrint(inPath, outPath, "A4", 96, "RGB", "portrait");
#elif FUZZ_MODE == FUZZ_DUMP
  // Both dump formats main.cpp accepts for -t: legacy schematic and Verilog-schematic.
  doDump(inPath, outPath + ".sch", "sch");
  resetModules();
  doDump(inPath, outPath + ".vs", "vs");
#endif

  resetModules();
  return 0;
}
