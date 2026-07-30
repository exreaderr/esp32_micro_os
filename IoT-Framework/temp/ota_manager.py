#!/usr/bin/env python3
import customtkinter as ctk
import json, os, subprocess, threading, shutil
from datetime import datetime

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")

# Пресеты для быстрого выбора размера раздела статики LittleFS
FS_PRESETS = {
    "Default (1.5MB FS)": "1507328",
    "No OTA (2MB FS)": "2097152",
    "Minimal (128KB FS)": "131072",
    "Custom": "0"
}

class OTAManager(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("IoT Build Master Gold Edition v11.2 (PRO MAX)")
        self.geometry("1250x950")

        # Определение путей относительно исполняемого скрипта
        self.base_dir = os.path.dirname(os.path.realpath(__file__))
        self.arduino_path = "/home/kiv/.arduino-IDE_2.3.8/arduino-ide"
        self.config_file = os.path.join(self.base_dir, "config.json")
        self.config = self.load_config()

        # Разметка сетки окон графического интерфейса
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # --- Левая панель (Sidebar) Профилей ---
        self.sidebar = ctk.CTkFrame(self, width=380)
        self.sidebar.grid(row=0, column=0, sticky="nsew")
        ctk.CTkLabel(self.sidebar, text="ПРОФИЛИ ПРОЕКТОВ", font=("Roboto", 22, "bold")).pack(pady=10)

        # Мониторинг сетевого пинга устройства в щите автоматизации
        self.status_frame = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        self.status_frame.pack(pady=5, padx=20, fill="x")
        self.status_led = ctk.CTkLabel(self.status_frame, text="●", font=("Roboto", 28), text_color="gray")
        self.status_led.pack(side="left", padx=10)
        self.status_text = ctk.CTkLabel(self.status_frame, text="Устройство: ???", font=("Roboto", 13, "bold"))
        self.status_text.pack(side="left")

        # Выпадающий список шаблонов из config.json
        self.project_list = ctk.CTkOptionMenu(self.sidebar, values=list(self.config.keys()) if self.config else ["Пусто"],
                                             command=self.load_project_details)
        self.project_list.pack(pady=5, padx=20, fill="x")

        # Динамическая генерация инпутов конфигурации
        self.entry_name = self.create_input_row("ID проекта (MQTT)", browse=False)
        self.entry_device_ip = self.create_input_row("IP устройства (Ping)", browse=False)
        self.entry_src = self.create_input_row("Путь к проекту (.ino)")
        self.entry_bin_dir = self.create_input_row("Путь к папке build")
        self.entry_ha = self.create_input_row("Путь к OTA HA")

        # Конфигуратор геометрии Flash-памяти LittleFS
        ctk.CTkLabel(self.sidebar, text="Размер LittleFS (bytes)", font=("Roboto", 11)).pack(padx=25, anchor="w")
        fs_row = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        fs_row.pack(padx=20, fill="x", pady=2)
        self.entry_size = ctk.CTkEntry(fs_row)
        self.entry_size.pack(side="left", fill="x", expand=True, padx=(0, 5))
        ctk.CTkButton(fs_row, text="🔍", width=35, command=self.auto_detect_fs_size).pack(side="right")

        self.preset_menu = ctk.CTkOptionMenu(self.sidebar, values=list(FS_PRESETS.keys()), command=self.apply_preset)
        self.preset_menu.pack(padx=20, fill="x", pady=2)

        self.entry_mqtt_ip = self.create_input_row("MQTT Брокер IP", False)
        self.entry_mqtt_user = self.create_input_row("Логин MQTT", False)
        self.entry_mqtt_pass = self.create_input_row("Пароль MQTT", False)
        self.entry_mqtt_pass.configure(show="*")
        self.entry_ver = self.create_input_row("Текущая версия прошивки", False)

        # Кнопки взаимодействия с профилями устройств
        ctk.CTkButton(self.sidebar, text="💾 Сохранить шаблон", fg_color="#3b5998", command=self.save_config).pack(pady=(15, 5), padx=20, fill="x")
        prof_ctrl = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        prof_ctrl.pack(pady=5, padx=20, fill="x")
        ctk.CTkButton(prof_ctrl, text="➕ Новый", fg_color="#27ae60", width=100, command=self.clear_fields).pack(side="left", expand=True, padx=(0, 5))
        ctk.CTkButton(prof_ctrl, text="🗑️ Удалить", fg_color="#c0392b", width=100, command=self.delete_project).pack(side="right", expand=True, padx=(5, 0))
        ctk.CTkButton(self.sidebar, text="📑 Открыть в Arduino IDE", fg_color="#2c3e50", command=self.open_ide).pack(pady=10, padx=20, fill="x")

        # --- Главная Рабочая Область Логирования ---
        self.main_content = ctk.CTkFrame(self, fg_color="transparent")
        self.main_content.grid(row=0, column=1, sticky="nsew", padx=20, pady=20)

        # Консольный текстовый терминал с цветовой разметкой тегов вывода
        self.log_box = ctk.CTkTextbox(self.main_content, font=("Courier New", 13))
        self.log_box.pack(expand=True, fill="both", pady=5)
        self.log_box._textbox.tag_config("error", foreground="#ff6b6b")
        self.log_box._textbox.tag_config("success", foreground="#51cf66")
        self.log_box._textbox.tag_config("info", foreground="#339af0")
        self.log_box._textbox.tag_config("warn", foreground="#fcc419")

        self.changelog_entry = ctk.CTkEntry(self.main_content, placeholder_text="История изменений (changelog) для релиза новой версии...")
        self.changelog_entry.pack(fill="x", pady=5)

        self.ctrl_frame = ctk.CTkFrame(self.main_content)
        self.ctrl_frame.pack(fill="x", pady=10)
        self.step_var = ctk.StringVar(value="0.1")
        ctk.CTkRadioButton(self.ctrl_frame, text="Патч-релиз (+0.1)", variable=self.step_var, value="0.1").pack(side="left", padx=20)
        ctk.CTkRadioButton(self.ctrl_frame, text="Мажорный-релиз (+1.0)", variable=self.step_var, value="1.0").pack(side="left")
        self.fs_var = ctk.BooleanVar(value=True)
        ctk.CTkCheckBox(self.ctrl_frame, text="Скомпилировать и обновить LittleFS", variable=self.fs_var).pack(side="left", padx=40)

        self.btn_run = ctk.CTkButton(self.main_content, text="🚀 ЗАПУСТИТЬ КОНВЕЙЕР АВТОСБОРКИ И ДЕПЛОЯ ОТА", height=60,
                                     font=("Roboto", 18, "bold"), fg_color="#1b6d21", command=self.start_build)
        self.btn_run.pack(fill="x", pady=5)

        # Панель быстрого отката версий
        svc_frame = ctk.CTkFrame(self.main_content, fg_color="transparent")
        svc_frame.pack(fill="x", pady=5)
        ctk.CTkButton(svc_frame, text="⏪ ВЕРНУТЬ БЭКАП (ROLLBACK)", fg_color="#8e44ad", command=self.rollback_version).pack(side="left", expand=True, padx=(0, 5))
        ctk.CTkButton(svc_frame, text="📂 ОТКРЫТЬ СЕТЕВУЮ ПАПКУ OTA", fg_color="#d35400", command=self.open_ota_folder).pack(side="left", expand=True, padx=5)

        if self.config:
            self.load_project_details(list(self.config.keys())[0])
        self.update_ping()

    # --- СЛОЙ ОПЕРАЦИОННОЙ ЛОГИКИ И ДИАГНОСТИКИ ---
    def log(self, text, tag=None):
        t = datetime.now().strftime("%H:%M:%S")
        self.log_box.insert("end", f"[{t}] {text}\n", tag)
        self.log_box.see("end")

    def auto_detect_fs_size(self):
        """Парсинг partitions.csv для вычисления точного размера LittleFS"""
        build_root = self.entry_bin_dir.get()
        if not os.path.exists(build_root):
            return
        csv_path = None
        direct_csv = os.path.join(build_root, "partitions.csv")
        if os.path.exists(direct_csv):
            csv_path = direct_csv
        else:
            for root, _, files in os.walk(build_root):
                if "partitions.csv" in files:
                    csv_path = os.path.join(root, "partitions.csv")
                    break
        if csv_path:
            try:
                with open(csv_path, "r") as f:
                    for line in f:
                        l = line.lower().strip()
                        if ("spiffs" in l or "littlefs" in l) and "data" in l:
                            parts = [p.strip() for p in l.split(",")]
                            if len(parts) >= 5:
                                sz_raw = parts[4]
                                val = int(sz_raw, 16) if sz_raw.startswith("0x") else int(sz_raw)
                                self.entry_size.delete(0, "end")
                                self.entry_size.insert(0, str(val))
                                short_p = os.path.relpath(csv_path, build_root)
                                self.log(f"РазмерLittleFS равен {val} байт. Извлечено из {short_p}", "info")
                                return
            except Exception as e:
                self.log(f"Ошибка чтения таблицы разделов CSV: {e}", "warn")
        else:
            self.log("Файл partitions.csv не найден в директории сборки build", "warn")

    def rollback_version(self):
        ha = self.entry_ha.get()
        bkp = os.path.join(ha, "backup")
        if os.path.exists(os.path.join(bkp, "version.json")):
            try:
                for f in os.listdir(bkp):
                    shutil.copy(os.path.join(bkp, f), os.path.join(ha, f))
                self.log("ОТКАТ ВЫПОЛНЕН: Резервная копия прошивки возвращена в PRODUCTION.", "success")
                self.load_project_details(self.entry_name.get())
            except Exception as e:
                self.log(f"Ошибка отката системы: {e}", "error")
        else:
            self.log("Резервный бэкам манифеста не обнаружен", "warn")

    def open_ota_folder(self):
        p = self.entry_ha.get()
        if os.path.exists(p):
            subprocess.Popen(['xdg-open', p])
        else:
            self.log("Папка развертывания ОТА недоступна", "error")

    def load_config(self):
        if os.path.exists(self.config_file):
            try:
                with open(self.config_file, "r") as f:
                    data = json.load(f)
                    return data if isinstance(data, dict) else {}
            except:
                return {}
        return {}

    def save_config(self):
        n = self.entry_name.get().strip()
        if not n:
            return self.log("Ошибка: Идентификатор ID проекта обязателен", "error")
        self.config[n] = {
            "src": self.entry_src.get(), "bin_dir": self.entry_bin_dir.get(), "ha": self.entry_ha.get(),
            "size": self.entry_size.get(), "mqtt_ip": self.entry_mqtt_ip.get(), "dev_ip": self.entry_device_ip.get(),
            "mqtt_user": self.entry_mqtt_user.get(), "mqtt_pass": self.entry_mqtt_pass.get()
        }
        with open(self.config_file, "w") as f:
            json.dump(self.config, f, indent=4)
        self.project_list.configure(values=list(self.config.keys()))
        self.log(f"Профиль устройства '{n}' успешно сохранен", "success")

    def delete_project(self):
        n = self.entry_name.get()
        if n in self.config:
            del self.config[n]
            with open(self.config_file, "w") as f:
                json.dump(self.config, f, indent=4)
            keys = list(self.config.keys())
            self.project_list.configure(values=keys if keys else ["Пусто"])
            self.load_project_details(keys[0]) if keys else self.clear_fields()
            self.log(f"Удален шаблон конфигурации: {n}", "warn")

    def clear_fields(self):
        for e in [self.entry_name, self.entry_src, self.entry_bin_dir, self.entry_ha, self.entry_size, self.entry_mqtt_ip,
                  self.entry_mqtt_user, self.entry_mqtt_pass, self.entry_device_ip, self.entry_ver]:
            e.delete(0, "end")

    def load_project_details(self, name):
        if name not in self.config:
            return
        p = self.config[name]
        self.project_list.set(name)
        mapping = [(self.entry_name, name), (self.entry_src, p.get("src", "")), (self.entry_bin_dir, p.get("bin_dir", "")),
                   (self.entry_ha, p.get("ha", "")), (self.entry_size, p.get("size", "")), (self.entry_mqtt_ip, p.get("mqtt_ip", "")),
                   (self.entry_mqtt_user, p.get("mqtt_user", "")), (self.entry_mqtt_pass, p.get("mqtt_pass", "")), (self.entry_device_ip, p.get("dev_ip", ""))]
        for e, v in mapping:
            e.delete(0, "end")
            e.insert(0, str(v))

        v_path = os.path.join(p.get("ha", ""), "version.json")
        if os.path.exists(v_path):
            try:
                with open(v_path, "r") as f:
                    self.entry_ver.delete(0, "end")
                    self.entry_ver.insert(0, json.load(f).get("version", "1.0.0"))
            except:
                pass
        self.after(200, self.auto_detect_fs_size)

    def create_input_row(self, label, browse=True):
        f = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        f.pack(padx=20, fill="x", pady=1)
        ctk.CTkLabel(f, text=label, font=("Roboto", 11)).pack(anchor="w")
        r = ctk.CTkFrame(f, fg_color="transparent")
        r.pack(fill="x")
        e = ctk.CTkEntry(r)
        e.pack(side="left", fill="x", expand=True, padx=(0, 5 if browse else 0))
        if browse:
            ctk.CTkButton(r, text="📁", width=35, command=lambda: self.browse(e)).pack(side="right")
        return e

    def browse(self, e):
        p = ctk.filedialog.askdirectory()
        if p:
            e.delete(0, "end")
            e.insert(0, p)

    def apply_preset(self, ch):
        if FS_PRESETS[ch] != "0":
            self.entry_size.delete(0, "end")
            self.entry_size.insert(0, FS_PRESETS[ch])

    def update_ping(self):
        def p_thread():
            ip = self.entry_device_ip.get()
            if ip:
                res = subprocess.run(['ping', '-c', '1', '-W', '1', ip], stdout=subprocess.DEVNULL)
                self.status_led.configure(text_color="#2ecc71" if res.returncode == 0 else "#e74c3c")
                self.status_text.configure(text="ONLINE (Связь OK)" if res.returncode == 0 else "OFFLINE (Разрыв)")
            self.after(5000, self.update_ping)
        threading.Thread(target=p_thread, daemon=True).start()

    def open_ide(self):
        p = self.entry_src.get()
        if os.path.exists(p):
            f = next((os.path.join(p, x) for x in os.listdir(p) if x.endswith(".ino")), p)
            subprocess.Popen([self.arduino_path, f])

    def start_build(self):
        self.log("Запуск DevOps конвейера компиляции...", "info")
        self.btn_run.configure(state="disabled", text="⚡ КОМПИЛЯЦИЯ ПАКЕТА...")
        threading.Thread(target=self.run_task, daemon=True).start()

    def run_task(self):
        do_fs = "true" if self.fs_var.get() else "false"
        script = os.path.join(self.base_dir, 'build_ota.sh')

        # Фиксация: Чейнджлог оборачивается для исключения разрыва индексов аргументов в Bash
        changelog_text = self.changelog_entry.get().strip() or "Periodic production build update"

        args = [
            self.entry_src.get(),
            self.entry_ha.get(),
            self.entry_size.get(),
            self.step_var.get(),
            self.entry_name.get().lower(),
            self.entry_mqtt_ip.get(),
            do_fs,
            self.entry_mqtt_user.get(),
            self.entry_mqtt_pass.get(),
            changelog_text,
            self.entry_ver.get(),
            self.entry_bin_dir.get()
        ]

        try :
            p = subprocess.Popen([script] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in p.stdout:
                line_str = line.strip()
                # Адаптивный интеллектуальный парсер цветовых тегов вывода Bash-скрипта
                tag = None
                if "УСПЕХ" in line_str or "SUCCESS" in line_str: tag = "success"
                elif "ОШИБКА" in line_str or "ERROR" in line_str or "FAILED" in line_str: tag = "error"
                elif "ВНИМАНИЕ" in line_str or "WARN" in line_str: tag = "warn"
                elif "ИНФО" in line_str or "INFO" in line_str: tag = "info"

                self.log(line_str, tag)
            p.wait()
            self.after(500, lambda: self.load_project_details(self.entry_name.get()))
        except Exception as e:
            self.log(f"Критический сбой DevOps-конвейера: {e}", "error")
        finally:
            self.btn_run.configure(state="normal", text="🚀 ЗАПУСТИТЬ КОНВЕЙЕР СБОРКИ")

if __name__ == "__main__":
    OTAManager().mainloop()
