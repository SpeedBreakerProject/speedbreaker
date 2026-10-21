// SPDX-License-Identifier: GPL-3.0-or-later
// SpeedBreaker Windows launcher: edits settings.toml and a few launch-time
// environment variables, then starts SpeedBreaker.exe. Put it next to
// SpeedBreaker.exe. Plain .NET Framework 4.x WinForms, no dependencies.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace SpeedBreakerLauncher
{
    enum Kind { Bool, Choice, Int, Float }

    class Opt
    {
        public string Table;
        public string Key;
        public string Label;
        public string Help;
        public Kind Kind;
        public string[] Tokens;   // what settings.toml holds
        public string[] Labels;   // what the list shows
        public string Default;    // token, "true"/"false", or a number
        public decimal Min, Max, Step;
        public int Decimals;
        public Control Ctl;

        public string FullKey { get { return Table + "." + Key; } }
    }

    // A small, line-preserving reader/writer for the handful of keys the game keeps in settings.toml.
    static class Toml
    {
        static readonly Regex TableRx = new Regex(@"^\s*\[\s*([A-Za-z0-9_\.\-]+)\s*\]\s*(#.*)?$");
        static readonly Regex KeyRx = new Regex(@"^\s*([A-Za-z0-9_\-]+)\s*=\s*(.*?)\s*$");

        static string CleanValue(string raw)
        {
            raw = raw.Trim();
            if (raw.Length > 0 && raw[0] == '"')
            {
                int end = raw.IndexOf('"', 1);
                return end > 0 ? raw.Substring(1, end - 1) : raw.Substring(1);
            }
            int hash = raw.IndexOf('#');
            if (hash >= 0) raw = raw.Substring(0, hash);
            return raw.Trim();
        }

        public static Dictionary<string, string> Read(string path)
        {
            Dictionary<string, string> d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (!File.Exists(path)) return d;
            string table = "";
            foreach (string line in File.ReadAllLines(path))
            {
                string t = line.Trim();
                if (t.Length == 0 || t[0] == '#') continue;
                Match m = TableRx.Match(line);
                if (m.Success) { table = m.Groups[1].Value; continue; }
                m = KeyRx.Match(line);
                if (m.Success) d[table + "." + m.Groups[1].Value] = CleanValue(m.Groups[2].Value);
            }
            return d;
        }

        // changes: "table.key" -> TOML value text. Existing lines are edited in place; missing keys are added to
        // their table (or a new table at the end). Everything else in the file stays as it is.
        public static void Write(string path, Dictionary<string, string> changes)
        {
            List<string> lines = new List<string>();
            if (File.Exists(path)) lines.AddRange(File.ReadAllLines(path));

            HashSet<string> done = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            Dictionary<string, int> lastOf = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            string table = "";
            for (int i = 0; i < lines.Count; i++)
            {
                Match tm = TableRx.Match(lines[i]);
                if (tm.Success)
                {
                    table = tm.Groups[1].Value;
                    lastOf[table] = i;
                    continue;
                }
                string trimmed = lines[i].Trim();
                if (trimmed.Length == 0 || trimmed[0] == '#') continue;
                lastOf[table] = i;
                Match km = KeyRx.Match(lines[i]);
                if (!km.Success) continue;
                string full = table + "." + km.Groups[1].Value;
                string value;
                if (changes.TryGetValue(full, out value))
                {
                    lines[i] = km.Groups[1].Value + " = " + value;
                    done.Add(full);
                }
            }

            // Missing keys, grouped by table.
            Dictionary<string, List<string>> missing = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (KeyValuePair<string, string> kv in changes)
            {
                if (done.Contains(kv.Key)) continue;
                int dot = kv.Key.IndexOf('.');
                string tbl = kv.Key.Substring(0, dot);
                string key = kv.Key.Substring(dot + 1);
                List<string> list;
                if (!missing.TryGetValue(tbl, out list)) { list = new List<string>(); missing[tbl] = list; }
                list.Add(key + " = " + kv.Value);
            }

            Dictionary<int, List<string>> inserts = new Dictionary<int, List<string>>();
            List<string> appended = new List<string>();
            foreach (KeyValuePair<string, List<string>> kv in missing)
            {
                int at;
                if (lastOf.TryGetValue(kv.Key, out at)) inserts[at] = kv.Value;
                else
                {
                    appended.Add("");
                    appended.Add("[" + kv.Key + "]");
                    appended.AddRange(kv.Value);
                }
            }
            List<int> order = new List<int>(inserts.Keys);
            order.Sort();
            order.Reverse();
            foreach (int at in order) lines.InsertRange(at + 1, inserts[at]);
            lines.AddRange(appended);

            string dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            if (File.Exists(path))
            {
                string bak = path + ".launcher-backup";
                if (!File.Exists(bak)) File.Copy(path, bak);
            }
            string tmp = path + ".launcher-tmp";
            File.WriteAllText(tmp, string.Join("\r\n", lines.ToArray()) + "\r\n", new UTF8Encoding(false));
            File.Copy(tmp, path, true);
            File.Delete(tmp);
        }
    }

    class MainForm : Form
    {
        readonly string exeDir;
        string settingsPath;
        readonly List<Opt> opts = new List<Opt>();
        readonly Dictionary<string, string> launcherCfg = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly string launcherIni;
        readonly ToolTip tips = new ToolTip();

        Label pathLabel;
        ComboBox audioDriver, audioBuffer, priority;
        CheckBox recordAudio;
        NumericUpDown recordFrom;
        TextBox extraEnv;

        public MainForm()
        {
            exeDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
            launcherIni = Path.Combine(exeDir, "SpeedBreakerLauncher.ini");
            settingsPath = DefaultSettingsPath();

            Text = "SpeedBreaker Launcher";
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(620, 600);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            Font = new Font("Segoe UI", 9f);
            tips.AutoPopDelay = 20000;

            DefineOptions();
            LoadLauncherIni();
            BuildUi();
            LoadValues();
        }

        // Where the game keeps settings.toml (runtime/user/paths.h and portable.h): in portable mode the data folder
        // beside the exe; otherwise $XDG_DATA_HOME, or $HOME (the current folder when HOME isn't set, which is the
        // usual case on Windows: the launcher starts the game in its own folder) + .local/share, then "speedbreaker".
        string DefaultSettingsPath()
        {
            if (File.Exists(Path.Combine(exeDir, "portable.txt")))
                return Path.Combine(Path.Combine(exeDir, "data"), "settings.toml");
            string xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            string home = Environment.GetEnvironmentVariable("HOME");
            string baseDir;
            if (!string.IsNullOrEmpty(xdg)) baseDir = xdg;
            else baseDir = Path.Combine(Path.Combine(string.IsNullOrEmpty(home) ? exeDir : home, ".local"), "share");
            string sb = Path.Combine(baseDir, "SpeedBreaker");
            string lower = Path.Combine(baseDir, "speedbreaker");
            // The game's folder name is lower-case on Windows; Windows doesn't care about case anyway.
            return Path.Combine(Directory.Exists(sb) ? sb : lower, "settings.toml");
        }

        void DefineOptions()
        {
            opts.Add(Bool("video", "fullscreen", "Fullscreen", "Borderless fullscreen. Off: a resizable window.", false));
            opts.Add(IntOpt("video", "window_width", "Window width (px)", "Window size when windowed.", 1280, 320, 16384, 10));
            opts.Add(IntOpt("video", "window_height", "Window height (px)", "Window size when windowed.", 720, 200, 16384, 10));
            opts.Add(Bool("video", "vsync", "V-Sync", "Show each frame on a display refresh. Off can tear but frames appear sooner.", true));
            opts.Add(Choice("video", "frame_rate", "Frame rate",
                "The game runs at 60 or 30 fps (it keeps its speed at 30). Auto drops to 30 only when the GPU can't hold 60. There is no uncapped mode.",
                new[] { "auto", "60", "30" }, new[] { "Auto", "60 fps", "30 fps" }, "60"));
            opts.Add(Choice("video", "internal_resolution", "Resolution scale (internal)",
                "Renders the 3D scene at a multiple of the console's 720p. 1.5x = 1080p, 2x = 1440p, 3x = 2160p. Auto picks from your screen.",
                new[] { "auto", "1x", "1.5x", "2x", "3x" }, new[] { "Auto", "1x (720p)", "1.5x (1080p)", "2x (1440p)", "3x (2160p)" }, "auto"));
            opts.Add(Bool("video", "anti_aliasing", "Anti-aliasing",
                "On/off only: an FXAA-style pass (the console used 2x MSAA). There are no MSAA levels in this build.", true));
            opts.Add(Choice("video", "anisotropic_filtering", "Anisotropic filtering",
                "Keeps roads and walls sharp at a distance. Auto: 16x on a dedicated GPU, 4x on integrated. Original: each texture's own setting, as on the 360.",
                new[] { "auto", "off", "2x", "4x", "8x", "16x", "game" }, new[] { "Auto", "Off", "2x", "4x", "8x", "16x", "Original" }, "auto"));
            opts.Add(Bool("video", "mipmaps", "Mipmaps", "Use the game's smaller textures in the distance so they don't shimmer.", true));
            opts.Add(Choice("video", "scaling", "Upscaling",
                "How the picture is enlarged to the window. FSR 1 helps when the window is much bigger than the internal resolution.",
                new[] { "bicubic", "linear", "fsr1" }, new[] { "Bicubic", "Bilinear", "FSR 1" }, "bicubic"));
            opts.Add(FloatOpt("video", "sharpening", "Sharpening", "0 turns it off, 1 is the strongest.", 0.3m, 0m, 1m, 0.05m, 2));
            opts.Add(Choice("video", "aspect_ratio", "Aspect ratio",
                "Fill Screen: a wider view on ultrawide, taller on 16:10 / 4:3, HUD kept at 16:9. Or 16:9 with bars.",
                new[] { "auto", "16:9" }, new[] { "Fill Screen", "16:9 (bars)" }, "auto"));
            opts.Add(IntOpt("video", "brightness", "Brightness", "-5..+5; 0 is the original picture.", 0, -5, 5, 1));
            opts.Add(Bool("video", "xbox_display_gamma", "Xbox 360 display gamma", "The gamma ramp a real 360 sends to a TV. Off: the flatter picture of v0.1.3 and earlier.", true));

            opts.Add(IntOpt("audio", "master_volume", "Master volume (%)", "Volume of all game audio.", 100, 0, 100, 5));
            opts.Add(Bool("audio", "mute", "Mute", "Silence all game audio.", false));
            opts.Add(Bool("audio", "mute_in_background", "Mute in background", "Silence the game while its window isn't focused.", false));

            opts.Add(Bool("advanced", "async_shaders", "Asynchronous shaders", "Skip an object for a frame or two while its shader compiles, instead of stuttering.", true));
            opts.Add(Bool("advanced", "performance_overlay", "Performance overlay", "Show frame rate and frame time in a corner.", false));
        }

        static Opt Bool(string t, string k, string label, string help, bool def)
        {
            Opt o = new Opt(); o.Table = t; o.Key = k; o.Label = label; o.Help = help; o.Kind = Kind.Bool;
            o.Default = def ? "true" : "false";
            return o;
        }
        static Opt Choice(string t, string k, string label, string help, string[] tokens, string[] labels, string def)
        {
            Opt o = new Opt(); o.Table = t; o.Key = k; o.Label = label; o.Help = help; o.Kind = Kind.Choice;
            o.Tokens = tokens; o.Labels = labels; o.Default = def;
            return o;
        }
        static Opt IntOpt(string t, string k, string label, string help, int def, int min, int max, int step)
        {
            Opt o = new Opt(); o.Table = t; o.Key = k; o.Label = label; o.Help = help; o.Kind = Kind.Int;
            o.Default = def.ToString(CultureInfo.InvariantCulture); o.Min = min; o.Max = max; o.Step = step;
            return o;
        }
        static Opt FloatOpt(string t, string k, string label, string help, decimal def, decimal min, decimal max, decimal step, int decimals)
        {
            Opt o = new Opt(); o.Table = t; o.Key = k; o.Label = label; o.Help = help; o.Kind = Kind.Float;
            o.Default = def.ToString(CultureInfo.InvariantCulture); o.Min = min; o.Max = max; o.Step = step; o.Decimals = decimals;
            return o;
        }

        Control MakeControl(Opt o)
        {
            if (o.Kind == Kind.Bool)
            {
                CheckBox c = new CheckBox();
                c.AutoSize = true;
                o.Ctl = c;
            }
            else if (o.Kind == Kind.Choice)
            {
                ComboBox c = new ComboBox();
                c.DropDownStyle = ComboBoxStyle.DropDownList;
                c.Width = 220;
                c.Items.AddRange(o.Labels);
                o.Ctl = c;
            }
            else
            {
                NumericUpDown n = new NumericUpDown();
                n.Width = 100;
                n.Minimum = o.Min; n.Maximum = o.Max; n.Increment = o.Step;
                n.DecimalPlaces = o.Decimals;
                o.Ctl = n;
            }
            tips.SetToolTip(o.Ctl, o.Help);
            return o.Ctl;
        }

        TabPage MakeTab(string title, string table, out TableLayoutPanel grid)
        {
            TabPage page = new TabPage(title);
            page.AutoScroll = true;
            grid = new TableLayoutPanel();
            grid.ColumnCount = 2;
            grid.AutoSize = true;
            grid.Dock = DockStyle.Top;
            grid.Padding = new Padding(10);
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            foreach (Opt o in opts)
            {
                if (o.Table != table) continue;
                AddRow(grid, o.Label, MakeControl(o), o.Help);
            }
            page.Controls.Add(grid);
            return page;
        }

        void AddRow(TableLayoutPanel grid, string label, Control c, string help)
        {
            Label l = new Label();
            l.Text = label;
            l.AutoSize = true;
            l.Margin = new Padding(3, 8, 3, 3);
            tips.SetToolTip(l, help);
            c.Margin = new Padding(3, 4, 3, 4);
            int row = grid.RowCount++;
            grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            grid.Controls.Add(l, 0, row);
            grid.Controls.Add(c, 1, row);
        }

        void BuildUi()
        {
            pathLabel = new Label();
            pathLabel.AutoSize = false;
            pathLabel.SetBounds(12, 8, 480, 32);
            pathLabel.Text = "";
            Controls.Add(pathLabel);

            Button change = new Button();
            change.Text = "Change...";
            change.SetBounds(500, 8, 108, 26);
            change.Click += delegate { ChooseSettingsFile(); };
            Controls.Add(change);

            TabControl tabs = new TabControl();
            tabs.SetBounds(8, 44, 604, 498);

            TableLayoutPanel g1, g2, g3;
            tabs.TabPages.Add(MakeTab("Graphics", "video", out g1));
            TabPage audioPage = MakeTab("Audio", "audio", out g2);
            tabs.TabPages.Add(audioPage);
            TabPage advPage = MakeTab("Advanced", "advanced", out g3);
            tabs.TabPages.Add(advPage);

            // Audio troubleshooting (launch-time only: environment variables for this run).
            Label sep = new Label();
            sep.Text = "Audio troubleshooting (applied at launch, not saved in settings.toml)";
            sep.AutoSize = true;
            sep.Font = new Font(Font, FontStyle.Bold);
            sep.Margin = new Padding(3, 18, 3, 3);
            int sr = g2.RowCount++;
            g2.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            g2.Controls.Add(sep, 0, sr);
            g2.SetColumnSpan(sep, 2);

            audioDriver = new ComboBox();
            audioDriver.DropDownStyle = ComboBoxStyle.DropDownList;
            audioDriver.Width = 220;
            audioDriver.Items.AddRange(new object[] { "Default (SDL decides)", "WASAPI", "DirectSound" });
            AddRow(g2, "Audio driver", audioDriver, "SDL_AUDIO_DRIVER. If sound is garbled on one driver, try the other.");

            audioBuffer = new ComboBox();
            audioBuffer.DropDownStyle = ComboBoxStyle.DropDownList;
            audioBuffer.Width = 220;
            audioBuffer.Items.AddRange(new object[] { "Default", "512 frames", "1024 frames", "2048 frames", "4096 frames" });
            AddRow(g2, "Device buffer", audioBuffer, "SDL_AUDIO_DEVICE_SAMPLE_FRAMES. A bigger buffer tolerates a late audio thread; the game sizes its queue from it.");

            recordAudio = new CheckBox();
            recordAudio.AutoSize = true;
            recordAudio.Text = "Record 30 s of the final mix to audio-dump.f32 and log levels";
            AddRow(g2, "Diagnostic recording", recordAudio, "NFSMW_AUDIO_DUMP + NFSMW_AUDIO_LOG. Raw 6-channel 48 kHz float, written before SDL touches it: if it is clean, the garbling is after the game's mixer.");

            recordFrom = new NumericUpDown();
            recordFrom.Minimum = 0; recordFrom.Maximum = 3600; recordFrom.Value = 20; recordFrom.Width = 100;
            AddRow(g2, "Start recording after (s)", recordFrom, "NFSMW_AUDIO_DUMP_FROM");

            // Advanced: launch-time extras.
            priority = new ComboBox();
            priority.DropDownStyle = ComboBoxStyle.DropDownList;
            priority.Width = 220;
            priority.Items.AddRange(new object[] { "Normal", "Above normal", "High" });
            AddRow(g3, "Process priority", priority, "Raises the whole game process. Can help if the audio thread is being starved.");

            extraEnv = new TextBox();
            extraEnv.Multiline = true;
            extraEnv.AcceptsReturn = true;
            extraEnv.ScrollBars = ScrollBars.Vertical;
            extraEnv.Size = new Size(330, 110);
            extraEnv.Font = new Font("Consolas", 9f);
            AddRow(g3, "Extra environment variables\r\n(NAME=VALUE per line)", extraEnv,
                "For any NFSMW_* switch from the source (e.g. NFSMW_VBLANK_LOCK=0). Environment variables win over settings.toml for that run.");

            Controls.Add(tabs);

            Button play = new Button();
            play.Text = "Save && Play";
            play.SetBounds(12, 552, 130, 34);
            play.Click += delegate { if (SaveAll()) Launch(); };
            Controls.Add(play);

            Button save = new Button();
            save.Text = "Save";
            save.SetBounds(150, 552, 90, 34);
            save.Click += delegate { if (SaveAll()) MessageBox.Show(this, "Saved to\r\n" + settingsPath, "SpeedBreaker Launcher"); };
            Controls.Add(save);

            Button reset = new Button();
            reset.Text = "Defaults";
            reset.SetBounds(248, 552, 90, 34);
            reset.Click += delegate { ApplyDefaults(); };
            Controls.Add(reset);

            Button folder = new Button();
            folder.Text = "Open folder";
            folder.SetBounds(346, 552, 100, 34);
            folder.Click += delegate { OpenFolder(); };
            Controls.Add(folder);

            Button quit = new Button();
            quit.Text = "Close";
            quit.SetBounds(520, 552, 90, 34);
            quit.Click += delegate { Close(); };
            Controls.Add(quit);
        }

        void SetControl(Opt o, string value)
        {
            if (o.Kind == Kind.Bool)
            {
                ((CheckBox)o.Ctl).Checked = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1" || value.Equals("on", StringComparison.OrdinalIgnoreCase);
            }
            else if (o.Kind == Kind.Choice)
            {
                int idx = Array.IndexOf(o.Tokens, value.ToLowerInvariant());
                if (idx < 0) idx = Array.IndexOf(o.Tokens, o.Default);
                ((ComboBox)o.Ctl).SelectedIndex = idx < 0 ? 0 : idx;
            }
            else
            {
                decimal v;
                if (!decimal.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out v)) v = decimal.Parse(o.Default, CultureInfo.InvariantCulture);
                NumericUpDown n = (NumericUpDown)o.Ctl;
                if (v < n.Minimum) v = n.Minimum;
                if (v > n.Maximum) v = n.Maximum;
                n.Value = v;
            }
        }

        string GetValueText(Opt o)
        {
            if (o.Kind == Kind.Bool) return ((CheckBox)o.Ctl).Checked ? "true" : "false";
            if (o.Kind == Kind.Choice)
            {
                int i = ((ComboBox)o.Ctl).SelectedIndex;
                return "\"" + o.Tokens[i < 0 ? 0 : i] + "\"";
            }
            NumericUpDown n = (NumericUpDown)o.Ctl;
            if (o.Kind == Kind.Int) return ((int)n.Value).ToString(CultureInfo.InvariantCulture);
            string s = n.Value.ToString(CultureInfo.InvariantCulture);
            return s.IndexOf('.') < 0 ? s + ".0" : s;
        }

        void LoadValues()
        {
            pathLabel.Text = "Settings file:\r\n" + settingsPath + (File.Exists(settingsPath) ? "" : "   (not created yet: the game makes it on first run; Save creates it now)");
            Dictionary<string, string> cur = Toml.Read(settingsPath);
            foreach (Opt o in opts)
            {
                string v;
                SetControl(o, cur.TryGetValue(o.FullKey, out v) ? v : o.Default);
            }
            // Launcher-only values.
            audioDriver.SelectedIndex = Math.Max(0, Array.IndexOf(new[] { "", "wasapi", "directsound" }, Cfg("audio_driver", "")));
            audioBuffer.SelectedIndex = Math.Max(0, Array.IndexOf(new[] { "", "512", "1024", "2048", "4096" }, Cfg("audio_buffer", "")));
            recordAudio.Checked = Cfg("record_audio", "0") == "1";
            int from; int.TryParse(Cfg("record_from", "20"), out from);
            recordFrom.Value = Math.Min(recordFrom.Maximum, Math.Max(recordFrom.Minimum, from));
            priority.SelectedIndex = Math.Max(0, Array.IndexOf(new[] { "normal", "abovenormal", "high" }, Cfg("priority", "normal")));
            extraEnv.Text = Cfg("extra_env", "").Replace("|", "\r\n");
        }

        void ApplyDefaults()
        {
            foreach (Opt o in opts) SetControl(o, o.Default);
            audioDriver.SelectedIndex = 0; audioBuffer.SelectedIndex = 0; recordAudio.Checked = false;
            recordFrom.Value = 20; priority.SelectedIndex = 0; extraEnv.Text = "";
        }

        string Cfg(string key, string def)
        {
            string v;
            return launcherCfg.TryGetValue(key, out v) ? v : def;
        }

        void LoadLauncherIni()
        {
            if (!File.Exists(launcherIni)) return;
            foreach (string line in File.ReadAllLines(launcherIni))
            {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                launcherCfg[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
            }
        }

        bool SaveAll()
        {
            try
            {
                Dictionary<string, string> changes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (Opt o in opts) changes[o.FullKey] = GetValueText(o);
                Toml.Write(settingsPath, changes);

                string[] drivers = { "", "wasapi", "directsound" };
                string[] buffers = { "", "512", "1024", "2048", "4096" };
                string[] prios = { "normal", "abovenormal", "high" };
                StringBuilder sb = new StringBuilder();
                sb.AppendLine("audio_driver=" + drivers[Math.Max(0, audioDriver.SelectedIndex)]);
                sb.AppendLine("audio_buffer=" + buffers[Math.Max(0, audioBuffer.SelectedIndex)]);
                sb.AppendLine("record_audio=" + (recordAudio.Checked ? "1" : "0"));
                sb.AppendLine("record_from=" + ((int)recordFrom.Value).ToString(CultureInfo.InvariantCulture));
                sb.AppendLine("priority=" + prios[Math.Max(0, priority.SelectedIndex)]);
                sb.AppendLine("extra_env=" + extraEnv.Text.Replace("\r\n", "|").Replace("\n", "|"));
                File.WriteAllText(launcherIni, sb.ToString(), new UTF8Encoding(false));
                launcherCfg.Clear();
                LoadLauncherIni();
                return true;
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "Couldn't save:\r\n" + e.Message, "SpeedBreaker Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        void Launch()
        {
            string exe = Path.Combine(exeDir, "SpeedBreaker.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show(this, "SpeedBreaker.exe isn't next to the launcher:\r\n" + exeDir, "SpeedBreaker Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(exe);
                psi.WorkingDirectory = exeDir;
                psi.UseShellExecute = false;

                string drv = Cfg("audio_driver", "");
                if (drv.Length > 0) psi.EnvironmentVariables["SDL_AUDIO_DRIVER"] = drv;
                string buf = Cfg("audio_buffer", "");
                if (buf.Length > 0) psi.EnvironmentVariables["SDL_AUDIO_DEVICE_SAMPLE_FRAMES"] = buf;
                if (Cfg("record_audio", "0") == "1")
                {
                    psi.EnvironmentVariables["NFSMW_AUDIO_DUMP"] = Path.Combine(exeDir, "audio-dump.f32");
                    psi.EnvironmentVariables["NFSMW_AUDIO_DUMP_FROM"] = Cfg("record_from", "20");
                    psi.EnvironmentVariables["NFSMW_AUDIO_DUMP_SECONDS"] = "30";
                    psi.EnvironmentVariables["NFSMW_AUDIO_LOG"] = "1";
                }
                foreach (string raw in Cfg("extra_env", "").Split(new char[] { '|' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    string line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#') continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    psi.EnvironmentVariables[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
                }

                Process p = Process.Start(psi);
                try
                {
                    string pr = Cfg("priority", "normal");
                    if (pr == "high") p.PriorityClass = ProcessPriorityClass.High;
                    else if (pr == "abovenormal") p.PriorityClass = ProcessPriorityClass.AboveNormal;
                }
                catch (Exception) { }
                Close();
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "Couldn't start the game:\r\n" + e.Message, "SpeedBreaker Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        void ChooseSettingsFile()
        {
            OpenFileDialog dlg = new OpenFileDialog();
            dlg.Title = "Choose the game's settings.toml";
            dlg.Filter = "settings.toml|settings.toml|All files|*.*";
            dlg.CheckFileExists = false;
            string dir = Path.GetDirectoryName(settingsPath);
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) dlg.InitialDirectory = dir;
            if (dlg.ShowDialog(this) == DialogResult.OK)
            {
                settingsPath = dlg.FileName;
                LoadValues();
            }
        }

        void OpenFolder()
        {
            string dir = Path.GetDirectoryName(settingsPath);
            if (string.IsNullOrEmpty(dir)) dir = exeDir;
            Directory.CreateDirectory(dir);
            Process.Start("explorer.exe", "\"" + dir + "\"");
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
