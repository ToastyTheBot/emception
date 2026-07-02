#include "quickjs/quickjs-libc.h"
#include "quickjs/quickjs.h"
#include "quickjspp.hpp"

#include <cstdlib>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <iostream>
#include <map>
#include <vector>

namespace fs = std::filesystem;

template <class T>
inline constexpr bool always_false_v = false;

class QuickNode {
  private:
    std::string m_version;
    qjs::Runtime m_runtime;
    qjs::Context m_context;
    std::map<fs::path, qjs::Value> m_require_cache;
    bool m_unhandled_rejection = false;

  public:
    QuickNode(std::string version)
      : m_version{std::move(version)}
      , m_runtime{}
      , m_context{m_runtime}
      , m_require_cache{}
    {
        m_context.onUnhandledPromiseRejection = [this](qjs::Value exc){
            m_unhandled_rejection = true;
            std::cerr << "Unhandled promise rejection: ";
            dump_exception();
        };
        add_global();
        add_require();
        add_console();
        add_process();
        add_assert();
        add_fs();
        add_path();
        setup_modules();
    }

    bool hadRejection() const { return m_unhandled_rejection; }

    bool execute_jobs() {
        bool success = true;
        while (m_runtime.isJobPending()) {
            try {
                m_runtime.executePendingJob();
            } catch(qjs::exception const & exc) {
                success = false;
                std::cerr << "Unhandled exception: ";
                dump_exception();
            } catch(std::exception const & e) {
                success = false;
                std::cerr << "Unhandled native exception: ";
                std::cerr << e.what() << "\n";
            }
        }
        return success;
    }

    std::string_view version() const {
        return m_version;
    }

    bool eval(const char * code) {
        try {
            m_context.eval(code, "<eval>", JS_EVAL_TYPE_GLOBAL);
            return true;
        } catch(qjs::exception const & exc) {
            std::cerr << "Unhandled exception: ";
            dump_exception();
        } catch(std::exception const & e) {
            std::cerr << "Unhandled native exception: ";
            std::cerr << e.what() << "\n";
        }
        return false;
    }

    bool evalFile(const char * filename) {
        try {
            auto code = readFile(filename);
            m_context.eval(code, filename, JS_EVAL_TYPE_GLOBAL);
            return true;
        } catch(qjs::exception const & exc) {
            std::cerr << "Unhandled exception: ";
            dump_exception();
        } catch(std::exception const & e) {
            std::cerr << "Unhandled native exception: ";
            std::cerr << e.what() << "\n";
        }
        return false;
    }

    // Evaluate an ES module (emscripten 3.1.74 ships src/compiler.mjs as ESM).
    // quicknode's require()/evalFile() are CommonJS-only, so .mjs entries are
    // compiled as modules with import.meta.url set; node: builtins are served by
    // the module loader installed in setup_modules().
    bool evalModule(const char * filename) {
        try {
            auto buf = qjs::detail::readFile(filename);
            if (!buf) throw std::runtime_error(std::string("Cannot read module: ") + filename);
            std::string code = std::move(*buf);
            if (code.size() >= 2 && code[0] == '#' && code[1] == '!') {
                auto nl = code.find('\n');
                code = (nl == std::string::npos) ? std::string{} : code.substr(nl + 1);
            }
            JSContext * c = m_context.ctx;
            JSValue func = JS_Eval(c, code.c_str(), code.size(), filename,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(func)) { dump_exception(); return false; }
            JSModuleDef * md = reinterpret_cast<JSModuleDef *>(JS_VALUE_GET_PTR(func));
            JSValue meta = JS_GetImportMeta(c, md);
            std::string uri = qjs::detail::toUri(filename);
            JS_SetPropertyStr(c, meta, "url", JS_NewString(c, uri.c_str()));
            JS_SetPropertyStr(c, meta, "main", JS_TRUE);
            JS_FreeValue(c, meta);
            JSValue ret = JS_EvalFunction(c, func); // consumes func; returns module eval promise
            if (JS_IsException(ret)) { dump_exception(); JS_FreeValue(c, ret); return false; }
            JS_FreeValue(c, ret);
            return true;
        } catch(qjs::exception const & exc) {
            std::cerr << "Unhandled exception: ";
            dump_exception();
        } catch(std::exception const & e) {
            std::cerr << "Unhandled native exception: ";
            std::cerr << e.what() << "\n";
        }
        return false;
    }

    qjs::Value global() {
        return m_context.global();
    }

  private:
    void dump_exception() {
        auto exc = m_context.getException();
        std::cerr << (exc.isError() ? "" : "Throw: ") << (std::string)exc << std::endl;
        if((bool)exc["stack"]) {
            std::cerr << (std::string)exc["stack"] << std::endl;
        }
    }

    auto print(std::ostream & o, qjs::rest<std::string> const & args) {
        auto it = args.begin(), end = args.end();
        if (it != end) o << *it;
        while (++it != end) o << " " << *it;
        o << "\n";
        return m_context.newValue(JS_UNDEFINED);
    };

    void add_global() {
        m_context.global()["global"] = m_context.global();
    }

    void add_path() {
        auto path = m_context.newObject();

        path.add("join", [this](qjs::rest<std::string> parts) -> std::string {
            fs::path joined;
            for (auto const & part : parts) {
                joined /= part;
            }
            if (joined.empty()) {
                return ".";
            }
            return fs::weakly_canonical(joined).string();
        });

        path.add("normalize", [this](std::string path) {
            return fs::weakly_canonical(path).string();
        });

        path.add("isAbsolute", [this](std::string path) {
            return fs::path{path}.is_absolute();
        });

        m_require_cache.insert({"path", path});
    }

    void add_fs() {
        auto fs = m_context.newObject();

        fs.add("existsSync", [this](std::string path) {
            return fs::exists(path);
        });
        fs.add("readFileSync", [this](std::string path) {
            auto buf = qjs::detail::readFile(path);
            if (!buf)
                throw std::runtime_error{std::string{"Can't read from file: "} + path};
            return *buf;
        });
        fs.add("writeFileSync", [this](std::string path, std::string content) {
            std::ofstream file{path};
            if (!file)
                throw std::runtime_error{std::string{"Can't write to file: "} + path};
            file << content;
        });

        m_require_cache.insert({"fs", fs});
    }

    void add_assert() {
        auto assert = m_context.newValue([](bool condition, qjs::rest<std::string> args) {
            if (!condition) std::runtime_error("assert failed");
        });
        m_require_cache.insert({"assert", assert});
    }

    void add_console() {
        auto console = m_context.newObject();
        m_context.global()["console"] = console;

        console.add("log", [this](qjs::rest<std::string> args) {
            return print(std::cout, args);
        });
        console.add("error", [this](qjs::rest<std::string> args) {
            return print(std::cerr, args);
        });
        console.add("warn", [this](qjs::rest<std::string> args) {
            return print(std::cerr, args);
        });

        m_require_cache.insert({"console", console});
    }

    void add_process() {
        auto process = m_context.newObject();
        m_context.global()["process"] = process;
        
        process.add("cwd", [](){
            return fs::current_path().string();
        });
        process["argv"] = std::vector<std::string>();
        process.add("exit", [](int status){
            std::exit(status);
        });

        auto env = m_context.newObject();
        process["env"] = env;
        for (char const * const * var = environ; *var; ++var) {
            std::string kv = *var;
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                env[kv.c_str()] = std::string();
            } else {
                env[kv.substr(0,eq).c_str()] = kv.substr(eq + 1);
            }
        }

        auto versions = m_context.newObject();
        process["versions"] = versions;
        versions["node"] = m_version;

        process["version"] = "v" + m_version;

        auto stdout = m_context.newObject();
        process["stdout"] = stdout;
        stdout.add("write", [this](qjs::rest<std::string> args) {
            return print(std::cout, args);
        });

        auto stderr = m_context.newObject();
        process["stderr"] = stderr;
        stderr.add("write", [this](qjs::rest<std::string> args) {
            return print(std::cerr, args);
        });

        m_require_cache.insert({"process", process});
    }

    // Install ESM support for the node: builtins emscripten 3.1.74's compiler.mjs
    // imports (fs/path/url/vm/assert). The heavy fs/path bits reuse the existing C
    // builtins; url/vm/URL are JS shims. vm is a SHARED-GLOBAL shim: emscripten's
    // applySettings() funnels settings into globalThis anyway (old emception ran
    // macros in the shared global via runInThisContext), so real context isolation
    // is unnecessary.
    void setup_modules() {
        auto qn = m_context.newObject();
        m_context.global()["__qn"] = qn;
        qn["fs"]      = m_require_cache.at("fs");
        qn["path"]    = m_require_cache.at("path");
        qn["assert"]  = m_require_cache.at("assert");
        qn["process"] = (qjs::Value) m_context.global()["process"];
        qn["console"] = (qjs::Value) m_context.global()["console"];

        eval(R"JS(
(function(){
  const q = globalThis.__qn;
  const p = q.path;
  if (!p.dirname)  p.dirname  = (s)=>{ s=String(s).replace(/\/+$/,''); const i=s.lastIndexOf('/'); return i<0?'.':(i===0?'/':s.slice(0,i)); };
  if (!p.basename) p.basename = (s,ext)=>{ s=String(s).replace(/\/+$/,''); let b=s.slice(s.lastIndexOf('/')+1); if(ext&&b.endsWith(ext)) b=b.slice(0,-ext.length); return b; };
  q.url = {
    fileURLToPath:(u)=>{ let s=(typeof u==='string')?u:((u&&u.href)?u.href:String(u)); return s.startsWith('file://')?decodeURIComponent(s.slice(7)):s; },
    pathToFileURL:(x)=>{ const href='file://'+String(x); return {href, toString(){return href;}}; },
  };
  globalThis.URL = class URL {
    constructor(input, base){ input=String(input);
      if (input.includes('://')){ this.href=input; return; }
      let b=base?String(base):''; let sc=b.startsWith('file://')?'file://':''; let bp=sc?b.slice(sc.length):b;
      let dir=bp.replace(/[^/]*$/,''); let res=dir+input; let out=[];
      for (const seg of res.split('/')){ if(seg==='.'||seg===''){ if(seg===''&&out.length===0) out.push(''); continue;} if(seg==='..'){ if(out.length>1) out.pop(); continue;} out.push(seg);}
      let norm=out.join('/'); if(res.endsWith('/')&&!norm.endsWith('/')) norm+='/'; this.href=(sc||'file://')+norm; }
    toString(){ return this.href; } get pathname(){ return this.href.replace(/^file:\/\//,''); }
  };
  // vm shim: emscripten's compiler evaluates its OWN trusted settings.js and
  // library-JS macros via node:vm. compiler.mjs's module functions (hasExportedSymbol,
  // etc.) close over the module globals that hold the Set-converted settings, so we must
  // NOT overwrite globalThis with a context's stale arrays. runInContext uses a
  // with()-scoped eval: macro code finds the context's helpers, while functions it calls
  // still read their own module globals. runInNewContext (settings.js) evals in the
  // shared global so `var` decls become globals (applySettings mirrors them). Sandboxed
  // inside WASM.
  const iEval = eval;
  q.vm = {
    // compileTimeContext IS globalThis: emscripten library JS files define helper
    // functions (e.g. wrapSyscallFunction) at top level that LATER library files' macros
    // reference, so those definitions must persist across runInContext calls. Node gets
    // this because the vm context is the global object for those evals. Using globalThis
    // as the context (indirect eval) gives the same persistence; the settings Sets built
    // on globalThis (compiler.mjs line ~54) are read correctly by module functions.
    createContext:(o)=>{ if (o) Object.assign(globalThis, o); return globalThis; },
    runInThisContext:(code)=> iEval(String(code)),
    runInContext:(code,ctx)=> { if (ctx && ctx !== globalThis) Object.assign(globalThis, ctx); try { return iEval(String(code)); } catch(e){ try{ globalThis.console.error("[vm.runInContext] " + (e&&e.name?e.name:"Error") + ": " + (e&&e.message?e.message:String(e)) + " | STACK: " + (e&&e.stack?e.stack:"")); }catch(_){} throw e; } },
    runInNewContext:(code)=> iEval(String(code)),
  };
  // Node stream methods emscripten's error path calls (process.stdout.once("drain",...)).
  for (const s of [globalThis.process && globalThis.process.stdout, globalThis.process && globalThis.process.stderr]) {
    if (!s) continue;
    if (!s.once) s.once = function(ev, cb){ if (typeof cb === "function") cb(); return s; };
    if (!s.on)   s.on   = function(){ return s; };
    if (!s.end)  s.end  = function(){ return s; };
  }
})();
)JS");

        m_context.moduleLoader = [](std::string_view name) -> qjs::Context::ModuleData {
            std::string n{name};
            std::string bare = n;
            if (bare.rfind("node:", 0) == 0) bare = bare.substr(5);
            static const std::map<std::string, std::vector<std::string>> exports = {
                {"fs",      {"existsSync","readFileSync","writeFileSync"}},
                {"path",    {"join","normalize","isAbsolute","dirname","basename"}},
                {"url",     {"fileURLToPath","pathToFileURL"}},
                {"vm",      {"createContext","runInContext","runInNewContext","runInThisContext"}},
                {"assert",  {}},
                {"process", {}},
                {"console", {}},
            };
            auto it = exports.find(bare);
            if (it != exports.end()) {
                std::string src = "const m = globalThis.__qn['" + bare + "'];\nexport default m;\n";
                for (auto const & e : it->second)
                    src += "export const " + e + " = m." + e + ";\n";
                return qjs::Context::ModuleData{ std::string("node:") + bare, src };
            }
            auto buf = qjs::detail::readFile(n);
            if (!buf) return qjs::Context::ModuleData{};
            return qjs::Context::ModuleData{ qjs::detail::toUri(n), *buf };
        };
    }

    std::string readFile(std::string const & file) {
        auto canonical = fs::weakly_canonical(file);
        auto buf = qjs::detail::readFile(canonical);
        if (!buf) throw std::runtime_error("Cannot find module '" + file + "'");
        auto code = std::move(*buf);
        if (!code.empty() && code[0] == '#') code = "//" + code;
        auto dirname = canonical.parent_path().string();
        auto filename = canonical.stem().string() + canonical.extension().string();
        return ""
        "("
            "() => {"
                "const module = { exports: {} };"
                "("
                    "(module, exports, require, __dirname, __filename) => {"
                        + code + "\n"
                    "}"
                ")(module, module.exports, (path) => require(path, \"" + dirname + "\"), \"" + dirname + "\", \"" + filename + "\");"
                "return module;"
            "}"
        ")()";
    }

    void add_require() {
        m_context.global().add("require", [this](std::string file, qjs::rest<std::string> rest){
            fs::path path;
            auto root = fs::current_path();
            if (!rest.empty()) {
                root = fs::path{rest[0]};
            }

            if (m_require_cache.count(file) > 0) {
                return m_require_cache.at(file);
            }

            if (file.substr(0, 1) == "/") {
                path = file;
            } else if (auto p = resolve_impl(file, root); p) {
                path = std::move(*p);
            } else {
                throw std::runtime_error("Cannot find module '" + file + "'");
            }
            
            auto canonical = fs::canonical(path);

            if (m_require_cache.count(canonical) == 0) {
                auto code = readFile(canonical);
                auto mod = m_context.eval(code, canonical.c_str(), JS_EVAL_TYPE_GLOBAL);
                if (!mod) mod = m_context.newObject();
                if (!mod["exports"]) mod["exports"] = m_context.newObject();
                m_require_cache.insert({ canonical, mod });
            }

            auto mod = m_require_cache.at(canonical);
            return (qjs::Value)mod["exports"];
        });
    }

    std::optional<fs::path> resolve_impl(std::string stem, fs::path const & root = fs::current_path(), bool recurse = true) {
        auto path = root / stem;
        if (fs::is_regular_file(path)) return path;
        if (auto p = fs::path{path}.concat(".js"); fs::is_regular_file(p)) return p;
        if (auto p = fs::path{path}.append("index.js"); fs::is_regular_file(p)) return p;
        if (auto p = fs::path{path}.append("package.json"); fs::is_regular_file(p)) {
            try {
                auto content = qjs::detail::readFile(p).value_or("{}");
                auto pkg = m_context.fromJSON(content);
                if ((bool)pkg["main"]) {
                    auto main = (std::string)pkg["main"];
                    if (auto p = fs::path{path}.append(main); fs::is_regular_file(p)) return p;
                    if (auto p = fs::path{path}.append(main).concat(".js"); fs::is_regular_file(p)) return p;
                }
            } catch (...) {
            }
        }
        if (root.filename() != "node_modules") {
            if (auto p = resolve_impl(stem, root / "node_modules", false); p) return p;
        }
        if (auto r = root.parent_path(); recurse && r != root) {
            if (auto p = resolve_impl(stem, r); p) return p;
        }
        return std::nullopt;
    }
};

int main(int argc, char ** argv) {
    bool success = true;
    QuickNode qn{"16.20.0"};

    std::vector<std::string> args(argc);
    for (size_t i = 0; i < args.size(); ++i) {
        args[i] = argv[i];
    }
    qn.global()["process"]["argv"] = args;

    if (argc == 3 && argv[1] == std::string_view{"-e"}) {
        success = success && qn.eval(argv[2]);
    } else if (argc >= 2 && argv[1][0] != '-') {
        std::string f = argv[1];
        bool isModule = f.size() > 4 && f.compare(f.size() - 4, 4, ".mjs") == 0;
        success = success && (isModule ? qn.evalModule(argv[1]) : qn.evalFile(argv[1]));
    } else if (argc == 2 && argv[1] == std::string_view{"--version"}) {
        std::cout << "v" << qn.version() << "\n";
        return 0;
    } else {
        std::cerr << "Welcome to QuickNode " << (std::string)qn.global()["process"]["version"] << ".\n";
        std::cerr << "Prompt is not available.\n";
        std::cerr << "\n";
        std::cerr << "Usage: quicknode --version\n";
        std::cerr << "Usage: quicknode -e <code>\n";
        std::cerr << "Usage: quicknode <script>\n";
        return 1;
    }

    success = success && qn.execute_jobs();
    if (qn.hadRejection()) success = false;

    return success ? 0 : 1;
}
