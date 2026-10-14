using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Diagnostics;
using System.Windows.Forms;
using System.Drawing;
using System.Threading.Tasks;
using System.Management;

public class Item { public string path; public string sha256; public long bytes; public string mode; public string component; public string blob; }
public class Package { public string version; public string game_sha256; public List<Item> files; public string scope; }
public class Record { public string path; public string installedHash; public string previousHash; public string backup; public bool managed; public bool restored; }
public class State { public string game; public string version; public string backupDirectory; public List<Record> records=new List<Record>(); public bool complete; public bool removed; public string executable; public string wrapper; }

public sealed class Slice : Stream {
 readonly Stream source; readonly long start,length; long position;
 public Slice(Stream source,long start,long length){this.source=source;this.start=start;this.length=length;}
 public override bool CanRead{get{return true;}} public override bool CanSeek{get{return true;}} public override bool CanWrite{get{return false;}}
 public override long Length{get{return length;}} public override long Position{get{return position;}set{Seek(value,SeekOrigin.Begin);}}
 public override int Read(byte[] b,int o,int n){source.Position=start+position;int r=source.Read(b,o,(int)Math.Min(n,length-position));position+=r;return r;}
 public override long Seek(long p,SeekOrigin origin){long target=origin==SeekOrigin.Begin?p:origin==SeekOrigin.Current?position+p:length+p;if(target<0||target>length)throw new IOException("Posicion invalida");position=target;return target;}
 public override void Flush(){} public override void SetLength(long v){throw new NotSupportedException();} public override void Write(byte[] b,int o,int n){throw new NotSupportedException();}
}

public static partial class Engine {
 [System.Runtime.InteropServices.DllImport("kernel32.dll",CharSet=System.Runtime.InteropServices.CharSet.Unicode,SetLastError=true)]
 static extern bool CreateHardLink(string target,string existing,IntPtr reserved);
 static void ValidateExecutable(string path){using(var s=File.OpenRead(path))using(var r=new BinaryReader(s)){
  if(s.Length<128||r.ReadUInt16()!=0x5a4d)throw new IOException("El archivo no es un ejecutable Windows.");
  s.Position=0x3c;uint offset=r.ReadUInt32();if(offset>s.Length-26)throw new IOException("Cabecera ejecutable invalida.");
  s.Position=offset;if(r.ReadUInt32()!=0x4550||r.ReadUInt16()!=0x8664)throw new IOException("El juego debe ser de 64 bits.");
  s.Position=offset+24;if(r.ReadUInt16()!=0x20b)throw new IOException("Se requiere ejecutable PE de 64 bits.");
 }}
 static void WriteManaged(State state,string relative,byte[] bytes,Action<string> log){
  Closed(state.executable);string target=Safe(state.game,relative);string digest;using(var stream=new MemoryStream(bytes))digest=Hash(stream);
  string old=File.Exists(target)?Hash(target):null;var record=new Record{path=relative,installedHash=digest,previousHash=old,managed=old!=digest};
  if(record.managed&&old!=null){record.backup=state.backupDirectory+"/generated/"+state.records.Count+"/"+relative;string b=Safe(state.game,record.backup);Directory.CreateDirectory(Path.GetDirectoryName(b));File.Copy(target,b,false);if(Hash(b)!=old)throw new IOException("Respaldo generado incorrecto.");}
  state.records.Add(record);WriteJson(Journal(state.game),state);if(!record.managed)return;
  Directory.CreateDirectory(Path.GetDirectoryName(target));string temp=target+".nr-install-new";if(File.Exists(temp))throw new IOException("Temporal de configuracion existente.");
  try{File.WriteAllBytes(temp,bytes);if(Hash(temp)!=digest)throw new IOException("Configuracion incorrecta.");Atomic(temp,target);}finally{if(File.Exists(temp))File.Delete(temp);}
 }
 public static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=16000000};
 public static string Hash(Stream s){using(var h=SHA256.Create())return BitConverter.ToString(h.ComputeHash(s)).Replace("-","").ToLowerInvariant();}
 public static string Hash(string p){using(var f=File.OpenRead(p))return Hash(f);}
 public static void WriteJson(string p,object value){Directory.CreateDirectory(Path.GetDirectoryName(p));string t=p+".new";File.WriteAllText(t,Json.Serialize(value),new UTF8Encoding(false));Atomic(t,p);}
 static void Atomic(string t,string p){
  for(int attempt=0;;attempt++){try{if(File.Exists(p))File.Replace(t,p,null);else File.Move(t,p);return;}
   catch(IOException error){if(attempt>=4)throw new IOException("No se pudo publicar el archivo: "+p,error);System.Threading.Thread.Sleep(100);}
  }
 }
 public static string Safe(string root,string relative){
  if(String.IsNullOrWhiteSpace(relative)||Path.IsPathRooted(relative)||relative.Contains(":")||relative.Split('/','\\').Any(x=>x==".."||x=="."||x==""))throw new IOException("Ruta de paquete invalida");
  string r=Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar;
  string p=Path.GetFullPath(Path.Combine(r,relative.Replace('/',Path.DirectorySeparatorChar)));
  if(!p.StartsWith(r,StringComparison.OrdinalIgnoreCase))throw new IOException("Ruta fuera de la carpeta elegida");
  for(string a=p;a!=null&&a.StartsWith(r,StringComparison.OrdinalIgnoreCase);a=Path.GetDirectoryName(a))if((File.Exists(a)||Directory.Exists(a))&&(File.GetAttributes(a)&FileAttributes.ReparsePoint)!=0)throw new IOException("No se admiten enlaces de directorio: "+a);
  if(Directory.Exists(root)&&(File.GetAttributes(root)&FileAttributes.ReparsePoint)!=0)throw new IOException("Carpeta enlazada no admitida");
  return p;
 }
 public static void Closed(string executable=null){if(!String.IsNullOrEmpty(executable)&&Process.GetProcessesByName(Path.GetFileNameWithoutExtension(executable)).Length!=0)throw new IOException("Cierra el juego elegido, tambien en el Administrador de tareas.");}
 public static bool SupportedHardware(string id){var parts=(id??"").ToUpperInvariant().Replace('\\','&').Split('&');return Array.IndexOf(parts,"VEN_8086")>=0&&(Array.IndexOf(parts,"DEV_E20B")>=0
#if !B580_ONLY
 ||Array.IndexOf(parts,"DEV_E20C")>=0
#endif
 );}
 static void Hardware(){using(var q=new ManagementObjectSearcher("SELECT PNPDeviceID FROM Win32_VideoController")){bool found=false;foreach(ManagementObject x in q.Get()){if(SupportedHardware(Convert.ToString(x["PNPDeviceID"])))found=true;}if(!found)throw new IOException("NR requiere Intel Arc B580 o B570 (B570 experimental, sin validacion fisica).");}}
 public static string Payload(Item item){if(item.blob==null||item.blob!=item.sha256||item.blob.Length!=64||item.blob.Any(c=>!((c>='0'&&c<='9')||(c>='a'&&c<='f'))))throw new IOException("Blob invalido");return "blobs/"+item.blob;}
 public static Package Verify(ZipArchive zip,Action<string> log){
  var e=zip.GetEntry("package.json");if(e==null)throw new IOException("Falta manifiesto");Package p;using(var r=new StreamReader(e.Open()))p=Json.Deserialize<Package>(r.ReadToEnd());
  if(p.files==null||p.files.Count==0||p.files.Count>5000)throw new IOException("Manifiesto invalido");
  var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
  if(zip.Entries.Select(x=>x.FullName).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=zip.Entries.Count)throw new IOException("Entradas ZIP duplicadas");
  log("Comprobando la integridad de los componentes...");int verified=0;var blobs=new HashSet<string>();
  foreach(var f in p.files){Safe(Path.GetTempPath(),f.path);if(!names.Add(f.path)||f.path.StartsWith("nr-general-installer/",StringComparison.OrdinalIgnoreCase)||f.sha256==null||f.sha256.Length!=64||f.bytes<=0)throw new IOException("Entrada de paquete invalida");
   var file=zip.GetEntry(Payload(f));if(file==null||file.Length!=f.bytes)throw new IOException("Tamano incorrecto: "+f.path);if(blobs.Add(f.blob))using(var s=file.Open())if(Hash(s)!=f.sha256)throw new IOException("Integridad incorrecta: "+f.path);
   verified++;if(verified%100==0)log("Verificacion: "+verified+" / "+p.files.Count);
  }
  if(zip.Entries.Count!=blobs.Count+1)throw new IOException("Archivos fuera del manifiesto");
  log("Integridad del paquete verificada: "+p.files.Count+" archivos.");return p;
 }
 static string Journal(string game){return Safe(game,"nr-general-installer/installation.json");}
 public static State Install(ZipArchive zip,Package package,string game,string bootstrap,Action<string> log,string wrapper="dxgi.dll"){
  string exe=Path.GetFullPath(game);game=Path.GetDirectoryName(exe);Closed(exe);Hardware();
  if(!File.Exists(exe)||Path.GetExtension(exe).ToLowerInvariant()!=".exe")throw new IOException("Selecciona el ejecutable real del juego.");
  Safe(game,Path.GetFileName(exe));ValidateExecutable(exe);
  if(Path.GetFileName(exe).Equals("re9.exe",StringComparison.OrdinalIgnoreCase))throw new IOException("Requiem requiere su lanzador especifico. Conserva su instalacion actual.");
  if(wrapper!="dxgi.dll"&&wrapper!="version.dll"&&wrapper!="winmm.dll")throw new IOException("DLL de carga no admitida.");
  if(package.files.Select(x=>x.path=="dxgi.dll"?wrapper:x.path).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=package.files.Count)throw new IOException("El metodo de carga duplica un archivo del paquete.");
  foreach(string alternative in new[]{"dxgi.dll","version.dll","winmm.dll","d3d12.dll","dbghelp.dll","wininet.dll","winhttp.dll"}){
   if(alternative==wrapper)continue;string candidate=Safe(game,alternative);
   if(File.Exists(candidate)){var info=FileVersionInfo.GetVersionInfo(candidate);if((info.ProductName??"").IndexOf("OptiScaler",StringComparison.OrdinalIgnoreCase)>=0)throw new IOException("Ya existe OptiScaler como "+alternative+". Elige ese metodo de carga o desinstala la version anterior.");}
  }
  bool history=File.Exists(Journal(game));
  if(history){var previous=Json.Deserialize<State>(File.ReadAllText(Journal(game)));if(!previous.removed||previous.game!=game)throw new IOException("Ya hay una instalacion registrada. Desinstalala antes de reinstalar.");}
  string reserved=Safe(game,"nr-general-installer");if(Directory.Exists(reserved)&&!history)throw new IOException("La carpeta nr-general-installer ya existe sin registro; no se sobrescribira.");
  var drive=new DriveInfo(Path.GetPathRoot(game));long payload=drive.DriveFormat=="NTFS"?package.files.GroupBy(x=>x.blob).Sum(x=>x.First().bytes):package.files.Sum(x=>x.bytes);
  long backupBytes=0;foreach(var item in package.files){string destination=Safe(game,item.path=="dxgi.dll"?wrapper:item.path);if(item.mode!="preserve"&&File.Exists(destination)&&Hash(destination)!=item.sha256)backupBytes+=new FileInfo(destination).Length;}
  if(drive.AvailableFreeSpace<payload+backupBytes+128000000)throw new IOException("Espacio insuficiente para instalar y respaldar.");
  State state=new State{game=game,executable=exe,wrapper=wrapper,version=package.version,backupDirectory="nr-general-installer/backups/"+DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N").Substring(0,8)};
  Directory.CreateDirectory(Safe(game,state.backupDirectory));if(history)File.Copy(Journal(game),Safe(game,state.backupDirectory+"/previous-installation.json"),false);WriteJson(Journal(game),state);
  try {
   int count=0;var written=new Dictionary<string,string>();foreach(var item in package.files){Closed(exe);string relative=item.path=="dxgi.dll"?wrapper:item.path;string target=Safe(game,relative);bool exists=File.Exists(target);string old=exists?Hash(target):null;
    var record=new Record{path=relative,installedHash=item.sha256,previousHash=old,managed=!(exists&&(old==item.sha256||item.mode=="preserve"))};
    if(record.managed&&exists){record.backup=state.backupDirectory+"/"+relative;string b=Safe(game,record.backup);Directory.CreateDirectory(Path.GetDirectoryName(b));File.Copy(target,b,false);if(Hash(b)!=old)throw new IOException("Error de respaldo: "+item.path);}
    state.records.Add(record);WriteJson(Journal(game),state);
    count++;if(count%50==0)log(item.component+": "+count+" / "+package.files.Count);
    if(!record.managed){if(old==item.sha256)written[item.sha256]=target;continue;}
    Directory.CreateDirectory(Path.GetDirectoryName(target));string temp=target+".nr-install-new";if(File.Exists(temp))throw new IOException("Archivo temporal previo: "+temp);
    try{string prior;bool linked=(relative.EndsWith(".bin")||relative.EndsWith(".dxil"))&&written.TryGetValue(item.sha256,out prior)&&File.Exists(prior)&&Hash(prior)==item.sha256&&CreateHardLink(temp,prior,IntPtr.Zero);
     if(!linked)using(var source=zip.GetEntry(Payload(item)).Open())using(var output=File.Create(temp))source.CopyTo(output);
     if(Hash(temp)!=item.sha256)throw new IOException("Error de copia: "+relative);Atomic(temp,target);written[item.sha256]=target;}

    finally{if(File.Exists(temp))File.Delete(temp);}
   }
   string ini=Safe(game,"OptiScaler.ini");
   if(Path.GetFileName(exe).Equals("Spider-Man2.exe",StringComparison.OrdinalIgnoreCase)){
    string config=File.ReadAllText(ini);var regex=new System.Text.RegularExpressions.Regex(@"(?m)^Dxgi\s*=.*$");
    if(regex.Matches(config).Count>1)throw new IOException("Hay ajustes Dxgi duplicados en el perfil Spider-Man.");
    string updated=regex.IsMatch(config)?regex.Replace(config,"Dxgi = false"):config+"\r\n[Spoofing]\r\nDxgi = false\r\n";
    WriteManaged(state,"OptiScaler.ini",Encoding.UTF8.GetBytes(updated),log);
   }
   var launchProfile=new Dictionary<string,object>{{"executable",Path.GetFileName(exe)},{"exe_sha256",Hash(exe)},{"dll",wrapper},{"dll_sha256",package.files.First(x=>x.path=="dxgi.dll").sha256},{"profile",Path.GetFileName(exe).Equals("Spider-Man2.exe",StringComparison.OrdinalIgnoreCase)?"Spider-Man 2 / referencia":"DX12 SDR / experimental"}};
#if PUBLIC_PACKAGE
   launchProfile["model_required"]=true;
#endif
   launchProfile["runtime_files"]=package.files.Where(x=>x.path.StartsWith("native_nr_runtime/",StringComparison.Ordinal)).Select(x=>new Dictionary<string,object>{{"path",x.path},{"sha256",x.sha256}}).ToArray();
   launchProfile["asset_manifests"]=package.files.Where(x=>x.path.StartsWith("native_nr_modes/",StringComparison.Ordinal)&&x.path.EndsWith("/assets.txt",StringComparison.Ordinal)).Select(x=>new Dictionary<string,object>{{"path",x.path},{"sha256",x.sha256}}).ToArray();
   WriteManaged(state,"nr-launcher/profile.json",Encoding.UTF8.GetBytes(Json.Serialize(launchProfile)),log);
   // The same compiled engine, without its appended archive, handles uninstall.
   File.Copy(bootstrap,Safe(game,"nr-general-installer/Uninstall.exe"),history);
   state.complete=true;WriteJson(Journal(game),state);log("Instalacion terminada. NR empieza desactivado.");return state;
  }catch {log("La instalacion fallo. Restaurando los cambios registrados...");Restore(state,log);throw;}
 }
 static void Restore(State state,Action<string> log){
  Closed(state.executable);foreach(var r in state.records.AsEnumerable().Reverse()){
   if(!r.managed||r.restored)continue;string target=Safe(state.game,r.path);
   if(File.Exists(target)&&Hash(target)!=r.installedHash){if(r.previousHash!=null&&Hash(target)==r.previousHash){r.restored=true;continue;}log("Conservado por cambios posteriores: "+r.path);continue;}
   if(r.backup!=null){string b=Safe(state.game,r.backup);if(!File.Exists(b)||Hash(b)!=r.previousHash)throw new IOException("Respaldo modificado: "+r.path);Directory.CreateDirectory(Path.GetDirectoryName(target));string temp=target+".nr-restore-new";if(File.Exists(temp))throw new IOException("Temporal de restauracion existente");File.Copy(b,temp,false);Atomic(temp,target);}
   else if(File.Exists(target))File.Delete(target);
   r.restored=true;WriteJson(Journal(state.game),state);
  }
  state.removed=state.records.All(x=>!x.managed||x.restored);WriteJson(Journal(state.game),state);
  log(state.removed?"Archivos anteriores restaurados. Los respaldos y el registro se conservan.":"Restauracion parcial: se conservaron archivos modificados despues de instalar.");
 }
 public static void Uninstall(string game,Action<string> log){
  if(File.Exists(game))game=Path.GetDirectoryName(Path.GetFullPath(game));State state=Json.Deserialize<State>(File.ReadAllText(Journal(game)));Closed(state.executable);
  if(!String.Equals(Path.GetFullPath(game),state.game,StringComparison.OrdinalIgnoreCase))throw new IOException("El registro pertenece a otra carpeta");
  Restore(state,log);
 }
 static List<Dictionary<string,object>> ValidateModelRoot(string game){
  var manifests=new List<Dictionary<string,object>>();int[,] geometry={{320,180,320,192},{512,288,512,320},{640,360,640,384},{960,540,960,576},{1280,720,1280,768}};
  for(int i=0;i<5;i++){
   string relative="native_nr_modes/assets-"+geometry[i,1]+"/assets.txt",manifest=Safe(game,relative),model=Path.GetDirectoryName(manifest);
   string[] rows=File.ReadAllLines(manifest);if(rows.Length<4||rows[0]!="NR_NATIVE_ASSETS 1")throw new IOException("Manifiesto de modelo invalido: "+relative);
   string expected="geometry "+geometry[i,1]+" "+geometry[i,0]+" "+geometry[i,3]+" "+geometry[i,2];if(rows[1]!=expected)throw new IOException("Geometria incompatible: "+relative);
   int count;var header=rows[2].Split(' ');if(header.Length!=2||header[0]!="files"||!int.TryParse(header[1],out count)||count<1||count>5000||rows.Length!=count+3)throw new IOException("Conteo de archivos invalido.");
   var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
   foreach(string row in rows.Skip(3)){var col=row.Split(new[]{' '},3);long bytes;
    if(col.Length!=3||!long.TryParse(col[0],out bytes)||bytes<0||col[1].Length!=64||!names.Add(col[2]))throw new IOException("Entrada de modelo invalida.");
    string file=Safe(model,col[2]);if(!File.Exists(file)||new FileInfo(file).Length!=bytes||Hash(file)!=col[1])throw new IOException("Modelo incompleto o alterado: "+col[2]);}
   manifests.Add(new Dictionary<string,object>{{"path",relative},{"sha256",Hash(manifest)}});
  }
  return manifests;
 }
 public static void RegisterLocalModel(string executable,Action<string> log,bool verifyOnly=false){
  string exe=Path.GetFullPath(executable),game=Path.GetDirectoryName(exe);Closed(exe);Hardware();ValidateExecutable(exe);
  var state=Json.Deserialize<State>(File.ReadAllText(Journal(game)));
  if(state.game!=game||state.executable!=exe||!state.complete||state.removed)throw new IOException("Instala primero este paquete publico en el juego elegido.");
  var manifests=ValidateModelRoot(game);
  var contract=Json.Deserialize<List<Item>>(File.ReadAllText(Safe(game,"nr-launcher/runtime-contract.json")));
  if(contract==null||contract.Count==0||contract.Count>32)throw new IOException("Contrato SDK invalido.");
  foreach(var item in contract){if(!item.path.StartsWith("native_nr_runtime/",StringComparison.Ordinal))throw new IOException("Ruta SDK invalida.");string file=Safe(game,item.path);if(!File.Exists(file)||new FileInfo(file).Length!=item.bytes||Hash(file)!=item.sha256)throw new IOException("Runtime local ausente o incompatible: "+item.path);}
  if(verifyOnly){log("Modelo local y runtime verificados; no se modificaron archivos ni Windows.");return;}
  var profile=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Safe(game,"nr-launcher/profile.json")));
  if(Hash(exe)!=(string)profile["exe_sha256"]||Hash(Safe(game,(string)profile["dll"]))!=(string)profile["dll_sha256"])throw new IOException("Ejecutable o DLL cambiaron.");
  profile["runtime_files"]=contract.Select(x=>new Dictionary<string,object>{{"path",x.path},{"sha256",x.sha256}}).ToArray();profile["asset_manifests"]=manifests.ToArray();profile["model_required"]=false;
  WriteManaged(state,"nr-launcher/profile.json",Encoding.UTF8.GetBytes(Json.Serialize(profile)),log);
  string ini=File.ReadAllText(Safe(game,"native_nr.ini"));var regex=new System.Text.RegularExpressions.Regex(@"(?m)^GlobalB1\s*=.*$");
  if(regex.Matches(ini).Count!=1)throw new IOException("Configuracion GlobalB1 ambigua.");
  WriteManaged(state,"native_nr.ini",Encoding.UTF8.GetBytes(regex.Replace(ini,"GlobalB1=1")),log);
  log("Modelo local registrado. NR sigue desactivado inicialmente; usa el lanzador NR.");
 }
}

public class InstallerForm : Form {
 TextBox folder=new TextBox(),log=new TextBox();Button install=new Button(),remove=new Button(),browse=new Button();string exe;bool busy;ComboBox wrapper=new ComboBox();
 public InstallerForm(string exe){this.exe=exe;Text="OptiScaler + Neural Rendering — DX12 / Arc B570 y B580";ClientSize=new Size(760,510);StartPosition=FormStartPosition.CenterScreen;MinimumSize=Size;Font=new Font("Segoe UI",10);
  var title=new Label{Text="OptiScaler + Neural Rendering — cinco modos",Font=new Font("Segoe UI",17,FontStyle.Bold),Left=20,Top=18,Width=715,Height=35};Controls.Add(title);
  Controls.Add(new Label{Text="DX12 / SDR · B580 referencia probada · B570 experimental sin prueba fisica\nCinco modos de 180p a 720p; inicio 320x180 con NR desactivado.\nReferencia: Spider-Man 2. Otros juegos: compatibilidad experimental.",Left=20,Top=65,Width=710,Height=90});
#if PUBLIC_PACKAGE
  foreach(Control control in Controls)if(control is Label&&control.Top==65)control.Text="PAQUETE PUBLICO SIN PESOS NI SDK EXPERIMENTAL. NR requiere aporte local.\nOptiScaler base puede usarse sin NR. Consulta LEEME-Publico.txt.\nB580 referencia; B570 experimental solo en la variante B570-B580.";
  var register=new Button{Text="Preparar NR",Left=637,Top=211,Width=105,Height=35};Controls.Add(register);
  register.Click+=async(s,e)=>{if(busy)return;busy=true;install.Enabled=remove.Enabled=browse.Enabled=folder.Enabled=wrapper.Enabled=register.Enabled=false;try{await GuidedSetup.Run(folder.Text.Trim(),this,Print);}catch(Exception error){Print("Error: "+error.Message);}finally{busy=false;install.Enabled=remove.Enabled=browse.Enabled=folder.Enabled=wrapper.Enabled=register.Enabled=true;}};
#endif
#if B580_ONLY
  Text="OptiScaler + NR — B580 / paquete publico";
#endif
  folder.SetBounds(20,164,615,30);folder.Text="";Controls.Add(folder);
  browse.Text="Elegir…";browse.SetBounds(643,164,96,30);browse.Click+=(s,e)=>{using(var d=new OpenFileDialog()){d.Title="Ejecutable real del juego DX12";d.Filter="Ejecutable (*.exe)|*.exe";if(d.ShowDialog()==DialogResult.OK)folder.Text=d.FileName;}};Controls.Add(browse);
  Controls.Add(new Label{Text="Metodo de carga",Left=465,Top=194,Width=160,Height=16});
  wrapper.DropDownStyle=ComboBoxStyle.DropDownList;wrapper.Items.AddRange(new object[]{"dxgi.dll","version.dll","winmm.dll"});wrapper.SelectedIndex=0;wrapper.SetBounds(465,214,160,30);Controls.Add(wrapper);
  install.Text="Instalar";install.SetBounds(20,208,180,38);remove.Text="Restaurar / desinstalar";remove.SetBounds(210,208,240,38);Controls.Add(install);Controls.Add(remove);
  log.Multiline=true;log.ReadOnly=true;log.ScrollBars=ScrollBars.Vertical;log.SetBounds(20,260,719,226);Controls.Add(log);
  install.Click+=async(s,e)=>await Run(false);remove.Click+=async(s,e)=>await Run(true);
  if(Path.GetFileName(exe).Equals("Uninstall.exe",StringComparison.OrdinalIgnoreCase)){folder.Text=Path.GetDirectoryName(Path.GetDirectoryName(exe));install.Enabled=false;}
  FormClosing+=(s,e)=>{if(busy){e.Cancel=true;Print("Espera a que termine la operacion para cerrar.");}};
 }
 void Print(string text){if(InvokeRequired){BeginInvoke(new Action<string>(Print),text);return;}log.AppendText(text+Environment.NewLine);}
 async Task Run(bool uninstall){
  string target=folder.Text.Trim();string dll=(string)wrapper.SelectedItem;if(target.Length==0){Print("Elige el ejecutable del juego.");return;}
  busy=true;install.Enabled=remove.Enabled=browse.Enabled=wrapper.Enabled=false;folder.Enabled=false;
  try{await Task.Run(()=>{Engine.Closed();if(uninstall){Engine.Uninstall(target,Print);return;}
   using(var f=File.OpenRead(exe)){long offset,length;Program.Footer(f,out offset,out length);using(var slice=new Slice(f,offset,length))using(var zip=new ZipArchive(slice,ZipArchiveMode.Read,true)){
    var package=Engine.Verify(zip,Print);string bootstrap=Path.Combine(Path.GetTempPath(),"nr-general-installer-"+Guid.NewGuid().ToString("N")+".exe");
    try{using(var source=File.OpenRead(exe))using(var output=File.Create(bootstrap)){byte[] b=new byte[1048576];long left=offset;while(left>0){int n=source.Read(b,0,(int)Math.Min(b.Length,left));if(n<=0)throw new EndOfStreamException();output.Write(b,0,n);left-=n;}}
     Engine.Install(zip,package,target,bootstrap,Print,dll);
    }finally{if(File.Exists(bootstrap))File.Delete(bootstrap);}
   }}
  });}catch(Exception error){Print("Error: "+error.Message);}finally{busy=false;remove.Enabled=browse.Enabled=folder.Enabled=wrapper.Enabled=true;install.Enabled=!Path.GetFileName(exe).Equals("Uninstall.exe",StringComparison.OrdinalIgnoreCase);}
 }
}
public static class Program {
 public static void Footer(Stream file,out long offset,out long length){
  if(file.Length<32)throw new IOException("No hay paquete incrustado");file.Position=file.Length-32;using(var r=new BinaryReader(file,Encoding.ASCII,true)){if(Encoding.ASCII.GetString(r.ReadBytes(16))!="NRSETUPGENV1PACK")throw new IOException("Paquete no encontrado");offset=r.ReadInt64();length=r.ReadInt64();if(offset<=0||length<=0||offset+length!=file.Length-32)throw new IOException("Paquete truncado");}
 }
 [STAThread] public static int Main(string[] args){try{
#if PUBLIC_PACKAGE
  if(args.Length==4&&args[0]=="--import-local"){Engine.ImportLocalModel(args[1],args[2],args[3],Console.WriteLine);return 0;}
  if(args.Length==2&&args[0]=="--profile-test"){Console.WriteLine(GuidedSetup.Profile(args[1]));return 0;}
  if(args.Length==4&&args[0]=="--extract-sdk-test"){var contract=Engine.Json.Deserialize<List<Item>>(File.ReadAllText(args[3]));Console.WriteLine(Engine.ExtractOfficialRuntime(args[1],args[2],contract));return 0;}
#endif
  if(args.Length==2&&args[0]=="--register-model"){Engine.RegisterLocalModel(args[1],Console.WriteLine);return 0;}
  if(args.Length==2&&args[0]=="--check-model"){Engine.RegisterLocalModel(args[1],Console.WriteLine,true);return 0;}
  if(args.Length==2&&args[0]=="--hardware-test"){Console.WriteLine(Engine.SupportedHardware(args[1])?"SUPPORTED":"UNSUPPORTED");return Engine.SupportedHardware(args[1])?0:1;}
  if(args.Length==2&&args[0]=="--preview"){Application.EnableVisualStyles();using(var form=new InstallerForm(Application.ExecutablePath)){form.ShowInTaskbar=false;form.StartPosition=FormStartPosition.Manual;form.Location=new Point(-32000,-32000);form.Show();Application.DoEvents();using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(0,0,form.Width,form.Height));bitmap.Save(args[1],System.Drawing.Imaging.ImageFormat.Png);}form.Close();}return 0;}
  if(args.Length==2&&args[0]=="--verify"){using(var f=File.OpenRead(args[1])){long o,l;Footer(f,out o,out l);using(var s=new Slice(f,o,l))using(var zip=new ZipArchive(s,ZipArchiveMode.Read))Engine.Verify(zip,Console.WriteLine);}return 0;}
  if((args.Length==4||args.Length==5)&&args[0]=="--install-test"){using(var f=File.OpenRead(args[1]))using(var zip=new ZipArchive(f,ZipArchiveMode.Read)){var p=Engine.Verify(zip,Console.WriteLine);Engine.Install(zip,p,args[2],args[3],Console.WriteLine,args.Length==5?args[4]:"dxgi.dll");}return 0;}
  if(args.Length==2&&args[0]=="--uninstall-test"){Engine.Uninstall(args[1],Console.WriteLine);return 0;}
  Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new InstallerForm(Application.ExecutablePath));return 0;
 }catch(Exception e){Console.Error.WriteLine(e.Message);return 1;}}
}
