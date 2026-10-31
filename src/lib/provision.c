/* Default system content: app entries in /Applications, sample documents,
 * media, and the example programs and scripts in /Scripts. Existing files are
 * never replaced, so user edits and deliberately installed program versions
 * survive kernel updates. */
#include "lib/provision.h"
#include "kernel/fs.h"
#include "lib/example_docs.h"
#include "lib/example_sheet.h"
#include "audio_example.h"
#include "native_example.h"
#include "sdk_examples.h"

/* Every built-in app has a node in /Applications, named like its launcher
 * entry. A missing one is created again on the next boot. */
static const char *const system_apps[]={
    "Files","Editor","Calculator","Paint","Image Viewer","Snake","Wordle","Terminal","Todo","Clock",
    "Calendar","Minesweeper","2048","Breakout","System Monitor","Settings","Browser","Media Player",
    "Writer","Spreadsheet","Help",
};

/* DOS programs on the DOS disk (tools/dos_apps.py makes it; make run attaches
 * it as /disk1). Each launcher file holds the path DOSBox starts. */
static const struct { const char *name, *target; } dos_app_launchers[]={
    {"FED Editor.dos","/disk1/DOS/FED.BAT"},
    {"NetHack.dos","/disk1/DOS/NETHACK.BAT"},
    {"Floppy Bird.dos","/disk1/DOS/FLPYBIRD.BAT"},
    {"Vitetris.dos","/disk1/DOS/VITETRIS.BAT"},
    {"Sudoku86.dos","/disk1/DOS/SUDOKU86.BAT"},
};

static int root_folder(const char *name){
    int id=fs_find_child(fs_root(),name);
    if(id<0)id=fs_mkdir(fs_root(),name);
    return fs_is_dir(id)?id:-1;
}

/* Older volumes kept app nodes at the root and programs in /Programs. Move
 * them once. A name already taken at the destination is never replaced:
 * a duplicate app node (it holds no data) is dropped, any other file stays
 * in /Programs, which then remains. */
static void migrate_layout(int apps,int scripts){
    int root=fs_root(),programs=fs_find_child(root,FS_LEGACY_SCRIPTS_DIR);
    for(int id=1;id<fs_node_slots();id++){
        if(apps>=0&&fs_is_app(id)&&fs_parent(id)==root){
            if(fs_find_child(apps,fs_name(id))<0)fs_move(id,apps);
            else fs_delete(id);
        }
        else if(scripts>=0&&fs_is_dir(programs)&&fs_valid(id)&&fs_parent(id)==programs&&
                !fs_holds_app(id)&&!fs_is_app(id)&&fs_find_child(scripts,fs_name(id))<0)
            fs_move(id,scripts);
    }
    if(scripts>=0&&fs_is_dir(programs)&&!fs_child_count(programs))fs_delete(programs);
}

void provision_system_files(char *scratch, unsigned capacity){
    int apps=root_folder(FS_APPS_DIR),scripts=root_folder(FS_SCRIPTS_DIR);
    migrate_layout(apps,scripts);
    for(unsigned i=0;apps>=0&&i<sizeof system_apps/sizeof *system_apps;i++)
        if(fs_find_child(apps,system_apps[i])<0)fs_create_app(apps,system_apps[i]);
    for(unsigned i=0;apps>=0&&i<sizeof dos_app_launchers/sizeof *dos_app_launchers;i++)
        if(fs_find_child(apps,dos_app_launchers[i].name)<0){
            int id=fs_create(apps,dos_app_launchers[i].name),length=kstrlen(dos_app_launchers[i].target);
            if(id>=0&&fs_write(id,dos_app_launchers[i].target,length)!=length)fs_delete(id);
        }
    int media=fs_find_child(fs_root(),"Media");if(media<0)media=fs_mkdir(fs_root(),"Media");
    if(media>=0&&fs_find_child(media,"chime.wav")<0){int id=fs_create(media,"chime.wav");if(id>=0)fs_write(id,(const char *)audio_example,sizeof audio_example);}
    int docs=fs_find_child(fs_root(),"Documents");
    if(docs>=0&&fs_find_child(docs,"budget.bsh")<0){
        unsigned size=example_budget_sheet((unsigned char *)scratch,capacity);
        if(size){int id=fs_create(docs,"budget.bsh");if(id>=0&&fs_write(id,scratch,(int)size)!=(int)size)fs_delete(id);}
    }
    if(docs>=0&&fs_find_child(docs,"budget.csv")<0){
        int id=fs_create(docs,"budget.csv");
        if(id>=0&&fs_write(id,example_budget_csv,kstrlen(example_budget_csv))!=kstrlen(example_budget_csv))fs_delete(id);
    }
    if(docs>=0&&fs_find_child(docs,"Spreadsheet guide.txt")<0){
        int id=fs_create(docs,"Spreadsheet guide.txt");
        if(id>=0&&fs_write(id,example_sheet_guide,kstrlen(example_sheet_guide))!=kstrlen(example_sheet_guide))fs_delete(id);
    }
    if(docs>=0&&fs_find_child(docs,"welcome.html")<0){
        const char *page=
            "<title>BaseOS guide</title><h1>BaseOS guide</h1>"
            "<p>Open apps and files with Ctrl+Space. Alt+Tab changes windows.</p>"
            "<h2>Files</h2><p>Ctrl+F filters this folder. Ctrl+1 through Ctrl+4 select Name, Type, Size or Modified sorting. "
            "Ctrl+C, Ctrl+X and Ctrl+V copy or move the selected file. Existing unrelated names are never replaced.</p>"
            "<h2>Documents</h2><p>Writer saves editable BWR files. Ctrl+Shift+E exports RTF; Ctrl+Shift+P exports Letter or A4 PDF. "
            "PDF is for viewing or printing on the host. Spreadsheet opens budget.bsh and exchanges CSV values.</p>"
            "<h2>Native apps</h2><p>Open a .bex program from Files or search. In Terminal, "
            "start /Scripts/docstats.bex /Documents/stats-sample.txt scans that document. S saves the report; Q exits. "
            "Existing installed examples are preserved when the kernel is updated.</p>"
            "<h2>Media and storage</h2><p>Media Player supports WAV, MP3 and bounded MPEG-1/MP2. "
            "Terminal df shows this volume's actual limits. The explicit large profile allows 16 MiB files and WAV/MP3 sources.</p>"
            "<h2>Browser</h2><p>Use HTTP addresses or file:///Documents/welcome.html. "
            "HTTPS, JavaScript and CSS layout are not supported. Never enter passwords over HTTP.</p>";
        int id=fs_create(docs,"welcome.html");if(id>=0)fs_write(id,page,kstrlen(page));
    }
    int dir=scripts;if(dir<0)return;
    if(fs_find_child(dir,"hello.bex")<0){int id=fs_create(dir,"hello.bex");if(id>=0)fs_write(id,(const char *)native_example,sizeof native_example);}
    if(fs_find_child(dir,"hello-c.bex")<0){int id=fs_create(dir,"hello-c.bex");if(id>=0)fs_write(id,(const char *)sdk_hello,sizeof sdk_hello);}
    if(fs_find_child(dir,"notebook.bex")<0){int id=fs_create(dir,"notebook.bex");if(id>=0)fs_write(id,(const char *)sdk_notebook,sizeof sdk_notebook);}
    if(fs_find_child(dir,"counter.bex")<0){int id=fs_create(dir,"counter.bex");if(id>=0)fs_write(id,(const char *)sdk_counter,sizeof sdk_counter);}
    if(fs_find_child(dir,"docstats.bex")<0){int id=fs_create(dir,"docstats.bex");if(id>=0)fs_write(id,(const char *)sdk_docstats,sizeof sdk_docstats);}
    if(fs_find_child(dir,"pointer.bex")<0){int id=fs_create(dir,"pointer.bex");if(id>=0)fs_write(id,(const char *)sdk_pointer,sizeof sdk_pointer);}
    if(fs_find_child(dir,"pointer-window.bex")<0){int id=fs_create(dir,"pointer-window.bex");if(id>=0)fs_write(id,(const char *)sdk_pointer_window,sizeof sdk_pointer_window);}
    if(docs>=0&&fs_find_child(docs,"stats-sample.txt")<0){
        unsigned length=example_stats_document(scratch,capacity);
        if(length&&length<=fs_file_limit()){
            int id=fs_create(docs,"stats-sample.txt");
            if(id>=0&&fs_write(id,scratch,(int)length)<0)fs_delete(id);
        }
    }
    const char *demo="10 PRINT \"BASIC: press a key while the picture draws\"\n20 LET A=0\n30 RECT A,A/2,8,8,A+32\n40 INKEY B\n50 IF B > 0 THEN 100\n60 WAIT 20\n70 LET A=A+2\n80 IF A < 150 THEN 30\n90 END\n100 PRINT B\n110 END\n";
    if(fs_find_child(dir,"demo.bas")<0){int id=fs_create(dir,"demo.bas");if(id>=0)fs_write(id,demo,kstrlen(demo));}
    const char *script="pwd\nls /\necho Scripts run one command per line.\ndf\n";
    if(fs_find_child(dir,"demo.sh")<0){int id=fs_create(dir,"demo.sh");if(id>=0)fs_write(id,script,kstrlen(script));}
}
