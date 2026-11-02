/*
 * Linux 安装器（C + GTK3）
 * 固定左右布局：左侧为物理硬盘列表，右侧实时显示 install.sh 输出（置灰日志区）。
 * 点击「开始安装」将选中的 /dev/设备名 作为参数传给 install.sh。
 * 选中系统盘时给出红色警示，安装前二次确认，结束后弹窗确认重启。
 * 编译：gcc installer.c -o installer $(pkg-config --cflags --libs gtk+-3.0)
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INSTALL_SCRIPT "install.sh"

typedef struct {
    char *name;       /* 设备名，如 nvme0n1 */
    char *size;       /* 容量 */
    char *model;      /* 型号 */
    gboolean is_system;  /* 是否为当前系统所在盘 */
} Disk;

static GtkWidget *listbox;
static GtkWidget *install_btn;
static GtkTextBuffer *log_buf;
static GPid child_pid = 0;

/* ---------------- 样式 ---------------- */
static const char *CSS = "\
window {\n\
    background-color: #eceef1;\n\
}\n\
.section-title {\n\
    font-size: 13px;\n\
    font-weight: bold;\n\
    color: #4b5563;\n\
    padding: 4px 6px;\n\
}\n\
.disk-panel {\n\
    background-color: #ffffff;\n\
    border: 1px solid #dcdfe4;\n\
    border-radius: 6px;\n\
}\n\
.log-panel {\n\
    background-color: #f3f4f6;\n\
    border: 1px solid #d1d5db;\n\
    border-radius: 6px;\n\
}\n\
.log-panel .section-title {\n\
    color: #4b5563;\n\
}\n\
row.disk-row {\n\
    background-color: #ffffff;\n\
    border-bottom: 1px solid #eef0f3;\n\
}\n\
row.disk-row:hover {\n\
    background-color: #f0f6ff;\n\
}\n\
row.disk-row:selected {\n\
    background-color: #d6e8ff;\n\
}\n\
.disk-dev {\n\
    font-weight: bold;\n\
    color: #111827;\n\
}\n\
.disk-meta {\n\
    font-size: 12px;\n\
    color: #6b7280;\n\
}\n\
.danger {\n\
    font-size: 12px;\n\
    font-weight: bold;\n\
    color: #dc2626;\n\
}\n\
.log-view,\n\
.log-view text {\n\
    background-color: #f3f4f6;\n\
    color: #374151;\n\
    font-family: monospace;\n\
    font-size: 12px;\n\
}\n\
#install-btn {\n\
    background-color: #d1d5db;\n\
    color: #000000;\n\
    border: none;\n\
    border-radius: 4px;\n\
    padding: 8px 14px;\n\
}\n\
#install-btn label {\n\
    color: #000000;\n\
}\n\
#install-btn:hover {\n\
    background-color: #9ca3af;\n\
}\n\
#install-btn:hover label {\n\
    color: #000000;\n\
}\n\
#install-btn:disabled {\n\
    background-color: #e5e7eb;\n\
}\n\
#install-btn:disabled label {\n\
    color: #9ca3af;\n\
}\n";

static void apply_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------------- 磁盘识别（基于 sysfs / procfs，不依赖 lsblk/findmnt） ---------------- */

/* 读取 sysfs/procfs 中某文件的一行内容（去掉换行），失败返回 NULL（调用方释放）。
 * 返回前做 UTF-8 校验，非法序列替换为替换字符，避免 Pango 渲染报错。 */
static char *read_sysfs_str(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;
    char buf[256] = "";
    if (fgets(buf, sizeof(buf), fp))
        buf[strcspn(buf, "\n")] = '\0';
    fclose(fp);
    if (!buf[0])
        return NULL;
    if (!g_utf8_validate(buf, -1, NULL))
        return g_utf8_make_valid(buf, -1);
    return g_strdup(buf);
}

static int is_excluded(const char *name)
{
    static const char *exclude[] = {
        "loop", "sr", "fd", "ram", "zram", "dm-", NULL
    };
    for (int i = 0; exclude[i]; i++) {
        if (strncmp(name, exclude[i], strlen(exclude[i])) == 0)
            return 1;
    }
    return 0;
}

/* 将 512 字节扇区数转换为可读容量，如 "476.9G" */
static char *format_size(unsigned long long sectors)
{
    double bytes = (double)sectors * 512.0;
    double gib = bytes / (1024.0 * 1024.0 * 1024.0);
    return g_strdup_printf("%.1fG", gib);
}

/* 通过 /proc/self/mounts 找到根分区所在物理盘设备名（如 vda），找不到返回 NULL */
static char *get_system_disk_name(void)
{
    FILE *fp = fopen("/proc/self/mounts", "r");
    if (!fp)
        return NULL;

    char dev[256], mnt[256], fstype[256], opts[512];
    char root_dev[256] = "";
    while (fscanf(fp, "%255s %255s %255s %511s", dev, mnt, fstype, opts) == 4) {
        if (strcmp(mnt, "/") == 0) {
            g_strlcpy(root_dev, dev, sizeof(root_dev));
            break;
        }
    }
    fclose(fp);
    if (!root_dev[0])
        return NULL;

    /* 取设备 basename，如 /dev/vda1 -> vda1 */
    const char *base = strrchr(root_dev, '/');
    base = base ? base + 1 : root_dev;

    /* 在 /sys/block/<disk>/<partition> 中找该分区所属的父盘 */
    DIR *dir = opendir("/sys/block");
    if (!dir)
        return NULL;

    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || is_excluded(ent->d_name))
            continue;
        char partpath[768];
        snprintf(partpath, sizeof(partpath), "/sys/block/%s/%s", ent->d_name, base);
        struct stat st;
        if (stat(partpath, &st) == 0) {
            result = g_strdup(ent->d_name);
            break;
        }
    }
    closedir(dir);
    return result;
}

/* 返回物理硬盘列表（元素为 Disk*，调用方负责释放） */
static GList *list_physical_disks(void)
{
    DIR *dir = opendir("/sys/block");
    if (!dir)
        return NULL;

    char *system_disk = get_system_disk_name();

    GList *disks = NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.' || is_excluded(name))
            continue;

        /* 真实物理/虚拟硬件磁盘有 device 目录，loop/ram 等虚拟设备没有 */
        char devpath[512];
        snprintf(devpath, sizeof(devpath), "/sys/block/%s/device", name);
        struct stat st;
        if (stat(devpath, &st) != 0)
            continue;

        Disk *d = g_new0(Disk, 1);
        d->name = g_strdup(name);

        char path[512];
        snprintf(path, sizeof(path), "/sys/block/%s/size", name);
        char *sz = read_sysfs_str(path);
        d->size = sz ? format_size(strtoull(sz, NULL, 10)) : g_strdup("?");

        snprintf(path, sizeof(path), "/sys/block/%s/device/model", name);
        char *model = read_sysfs_str(path);
        if (model) {
            g_strstrip(model);
            if (model[0])
                d->model = model;            /* 所有权转移给 d->model，不再释放 */
            else {
                d->model = g_strdup("未知型号");
                g_free(model);
            }
        } else {
            d->model = g_strdup("未知型号");
        }

        d->is_system = (system_disk && strcmp(name, system_disk) == 0);

        g_free(sz);
        disks = g_list_append(disks, d);
    }
    closedir(dir);
    g_free(system_disk);
    return disks;
}

static void disk_free(Disk *d)
{
    if (!d)
        return;
    g_free(d->name);
    g_free(d->size);
    g_free(d->model);
    g_free(d);
}

/* 深拷贝一份 Disk，随 row 生命周期释放 */
static Disk *disk_dup(const Disk *src)
{
    Disk *d = g_new0(Disk, 1);
    d->name = g_strdup(src->name);
    d->size = g_strdup(src->size);
    d->model = g_strdup(src->model);
    d->is_system = src->is_system;
    return d;
}

static void free_disk_list(GList *disks)
{
    for (GList *it = disks; it; it = it->next)
        disk_free(it->data);
    g_list_free(disks);
}

/* ---------------- 日志 ---------------- */
static void log_msg(const char *text)
{
    if (!log_buf)
        return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, text, -1);
    gtk_text_buffer_insert(log_buf, &end, "\n", -1);
}

/* ---------------- 子进程输出 ---------------- */
static gboolean on_pipe_data(GIOChannel *channel, GIOCondition cond, gpointer data)
{
    (void)data;
    if (cond & (G_IO_ERR | G_IO_HUP)) {
        gchar *line = NULL;
        GError *err = NULL;
        while (g_io_channel_read_line(channel, &line, NULL, NULL, &err)
               == G_IO_STATUS_NORMAL) {
            g_strchomp(line);
            log_msg(line);
            g_free(line);
        }
        if (err)
            g_error_free(err);
        return G_SOURCE_REMOVE;
    }

    gchar *line = NULL;
    gsize len = 0;
    GError *err = NULL;
    GIOStatus st = g_io_channel_read_line(channel, &line, &len, NULL, &err);
    if (st == G_IO_STATUS_NORMAL) {
        g_strchomp(line);
        log_msg(line);
        g_free(line);
    }
    if (err)
        g_error_free(err);
    return G_SOURCE_CONTINUE;
}

static void on_child_exit(GPid pid, gint status, gpointer data)
{
    (void)pid;
    (void)data;

    g_spawn_close_pid(child_pid);
    child_pid = 0;

    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "安装完成，是否重启系统？");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "脚本退出码: %d", status);

    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (resp == GTK_RESPONSE_YES) {
        log_msg("正在重启系统…");
        system("reboot");
    }

    gtk_widget_set_sensitive(install_btn, TRUE);
}

/* ---------------- 安装动作 ---------------- */
static void do_install(Disk *disk)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "调用脚本: %s /dev/%s", INSTALL_SCRIPT, disk->name);
    log_msg(msg);

    char app[256];
    snprintf(app, sizeof(app), "./%s", INSTALL_SCRIPT);
    gchar *disk_path = g_strdup_printf("/dev/%s", disk->name);
    gchar *argv[] = { app, disk_path, NULL };

    gint stdout_fd = -1, stderr_fd = -1;
    GError *err = NULL;

    gtk_widget_set_sensitive(install_btn, FALSE);

    if (!g_spawn_async_with_pipes(
            NULL, argv, NULL,
            G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
            NULL, NULL, &child_pid, NULL, &stdout_fd, &stderr_fd, &err)) {
        char emsg[512];
        snprintf(emsg, sizeof(emsg), "执行失败: %s", err ? err->message : "未知错误");
        log_msg(emsg);
        if (err)
            g_error_free(err);
        gtk_widget_set_sensitive(install_btn, TRUE);
        g_free(disk_path);
        return;
    }

    g_child_watch_add(child_pid, on_child_exit, NULL);

    GIOChannel *ch_out = g_io_channel_unix_new(stdout_fd);
    GIOChannel *ch_err = g_io_channel_unix_new(stderr_fd);
    g_io_add_watch(ch_out, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pipe_data, NULL);
    g_io_add_watch(ch_err, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pipe_data, NULL);
    g_io_channel_unref(ch_out);
    g_io_channel_unref(ch_err);

    g_free(disk_path);
}

static void on_install_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!row) {
        log_msg("错误：请先在左侧选择目标硬盘。");
        return;
    }
    Disk *disk = g_object_get_data(G_OBJECT(row), "disk");

    /* 二次确认，系统盘额外警示 */
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL,
        disk->is_system ? GTK_MESSAGE_WARNING : GTK_MESSAGE_QUESTION,
        disk->is_system ? GTK_BUTTONS_YES_NO : GTK_BUTTONS_OK_CANCEL,
        "确认开始安装？");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "目标磁盘: /dev/%s", disk->name);

    if (disk->is_system) {
        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        GtkWidget *warn = gtk_label_new(
            "⚠ 警告：所选磁盘是当前系统所在盘，继续操作将导致系统被覆盖。");
        gtk_widget_set_margin_top(warn, 6);
        gtk_widget_set_margin_bottom(warn, 6);
        gtk_widget_set_halign(warn, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(warn), TRUE);
        gtk_box_pack_start(GTK_BOX(content), warn, FALSE, FALSE, 0);
        gtk_widget_show(warn);
    }

    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    gboolean ok = disk->is_system ? (resp == GTK_RESPONSE_YES)
                                  : (resp == GTK_RESPONSE_OK);
    if (ok)
        do_install(disk);
}

/* ---------------- UI 构建 ---------------- */
static GtkWidget *make_disk_row(Disk *disk)
{
    char path[256];
    snprintf(path, sizeof(path), "/dev/%s", disk->name);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "disk-row");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget *dev_label = gtk_label_new(path);
    gtk_label_set_xalign(GTK_LABEL(dev_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dev_label), "disk-dev");

    gchar *meta = g_strdup_printf("%s  ·  %s", disk->size, disk->model);
    GtkWidget *meta_label = gtk_label_new(meta);
    gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(meta_label), "disk-meta");
    g_free(meta);

    gtk_box_pack_start(GTK_BOX(box), dev_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), meta_label, FALSE, FALSE, 0);

    if (disk->is_system) {
        GtkWidget *danger = gtk_label_new("⚠ 系统盘");
        gtk_label_set_xalign(GTK_LABEL(danger), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(danger), "danger");
        gtk_box_pack_start(GTK_BOX(box), danger, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "disk", disk_dup(disk),
                           (GDestroyNotify)disk_free);
    return row;
}

static GtkWidget *make_section_title(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "section-title");
    return label;
}

/* 清空并重新扫描、填充硬盘列表；首次填充与刷新按钮共用 */
static void repopulate_disk_list(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    GList *disks = list_physical_disks();
    gboolean has_disks = (disks != NULL);
    gboolean first = TRUE;
    for (GList *it = disks; it; it = it->next) {
        Disk *d = it->data;
        GtkWidget *row = make_disk_row(d);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
        gtk_widget_show_all(row);
        if (first) {
            gtk_list_box_select_row(GTK_LIST_BOX(listbox), GTK_LIST_BOX_ROW(row));
            first = FALSE;
        }
    }

    gtk_widget_set_sensitive(install_btn, has_disks);
    free_disk_list(disks);
    log_msg(has_disks ? "已刷新硬盘列表。" : "未检测到物理硬盘。");
}

static void on_refresh_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    repopulate_disk_list();
}

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    apply_css();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Linux 安装器");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 440);
    gtk_container_set_border_width(GTK_CONTAINER(window), 14);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* 固定左右布局：不可拖动 */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_add(GTK_CONTAINER(window), hbox);

    /* ---------- 左：硬盘列表 ---------- */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left, 270, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(left), "disk-panel");

    /* 标题栏：左侧标题，右侧刷新图标按钮 */
    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(title_row), make_section_title("选择安装硬盘"),
                       TRUE, TRUE, 0);
    GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh",
                                                           GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(refresh_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(refresh_btn, "刷新磁盘列表");
    gtk_widget_set_name(refresh_btn, "refresh-btn");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(title_row), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), title_row, FALSE, FALSE, 0);

    listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);

    GtkWidget *left_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(left_scroll), listbox);
    gtk_box_pack_start(GTK_BOX(left), left_scroll, TRUE, TRUE, 0);

    install_btn = gtk_button_new_with_label("开始安装");
    gtk_widget_set_name(install_btn, "install-btn");
    gtk_widget_set_margin_start(install_btn, 6);
    gtk_widget_set_margin_end(install_btn, 6);
    gtk_widget_set_margin_bottom(install_btn, 8);
    g_signal_connect(install_btn, "clicked", G_CALLBACK(on_install_clicked), NULL);

    /* 强制按钮文字为黑色，避免主题覆盖 */
    GtkWidget *btn_label = gtk_bin_get_child(GTK_BIN(install_btn));
    if (GTK_IS_LABEL(btn_label)) {
        GdkRGBA black = { 0.0, 0.0, 0.0, 1.0 };
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_widget_override_color(btn_label, GTK_STATE_FLAG_NORMAL, &black);
        gtk_widget_override_color(btn_label, GTK_STATE_FLAG_PRELIGHT, &black);
        gtk_widget_override_color(btn_label, GTK_STATE_FLAG_ACTIVE, &black);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }

    gtk_box_pack_start(GTK_BOX(left), install_btn, FALSE, FALSE, 0);

    /* ---------- 右：脚本输出（置灰） ---------- */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(right), "log-panel");

    gtk_box_pack_start(GTK_BOX(right), make_section_title("安装日志"),
                       FALSE, FALSE, 0);

    GtkWidget *log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(log_view), "log-view");
    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    GtkWidget *right_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(right_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(right_scroll), log_view);
    gtk_box_pack_start(GTK_BOX(right), right_scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), left, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), right, TRUE, TRUE, 0);

    /* 首次填充硬盘列表（log_buf 已就绪） */
    repopulate_disk_list();

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}