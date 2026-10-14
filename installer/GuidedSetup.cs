using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Collections.Generic;
using System.Net;
using System.Threading.Tasks;
using System.Windows.Forms;

public static class GuidedSetup {
 public static string Profile(string executable){switch(Path.GetFileName(executable).ToLowerInvariant()){
  case "spider-man2.exe":return "Spider-Man 2 / referencia general; verificar version y temporal en juego";
  case "re9.exe":return "Requiem / requiere integracion especifica; no aplicar perfil Spider-Man";
  case "re2.exe":case "re3.exe":case "re4.exe":return "RE Engine / experimental, DX12 requerido; temporal no validado en este titulo";
  default:return "DX12 SDR / experimental; motor no confirmado";
 }}
 public static async Task Run(string executable,IWin32Window owner,Action<string> log){
  string exe=Path.GetFullPath(executable),game=Path.GetDirectoryName(exe);Engine.Closed(exe);log(Profile(exe));
  if(Path.GetFileName(exe).Equals("re9.exe",StringComparison.OrdinalIgnoreCase))throw new IOException("Requiem conserva su instalador especifico con REFramework. No se instalara otro perfil.");
  if(!File.Exists(Path.Combine(game,"nr-general-installer/installation.json")))throw new IOException("Primero pulsa Instalar para este mismo ejecutable.");
  MessageBox.Show(owner,"NR necesita un paquete de modelo local compatible, obtenido con autorizacion. No descargamos pesos ni extraemos DLLs.\n\nSelecciona la carpeta que contiene native_nr_modes (los cinco modos). Si falta el runtime compatible, se ofrecera descargarlo de Microsoft.\n\nSe conservara NR apagado hasta que lo actives en el juego.","Preparacion guiada de NR",MessageBoxButtons.OK,MessageBoxIcon.Information);
  string source;using(var dialog=new FolderBrowserDialog()){dialog.Description="Carpeta del paquete local: debe contener native_nr_modes con los cinco modos";dialog.SelectedPath=game;if(dialog.ShowDialog(owner)!=DialogResult.OK){log("Preparacion cancelada; no se cambio el modelo.");return;}source=dialog.SelectedPath;}
  if(Path.GetFileName(source).Equals("native_nr_modes",StringComparison.OrdinalIgnoreCase))source=Path.GetDirectoryName(source);
  log("Comprobando los archivos del modelo local...");await Task.Run(()=>Engine.CheckModelSource(source));
  var runtime=Engine.RuntimeContract(exe);bool ready=runtime.All(x=>Engine.FileMatches(game,x));
  string runtimeRoot=source,temp=null;
  try{
   if(!ready&&!runtime.All(x=>Engine.FileMatches(source,x))){
    if(MessageBox.Show(owner,"Falta el runtime Microsoft Direct3D 1.721.3-preview.\n\nSe descargara desde api.nuget.org, sin pesos ni archivos del juego. Antes de instalar se mostrara su licencia para aceptarla.\n\nAutorizar la descarga?","Dependencia oficial de Microsoft",MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes){log("Descarga no autorizada; NR sigue sin preparar.");return;}
    temp=Path.Combine(Path.GetTempPath(),"nr-sdk-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(temp);string zip=Path.Combine(temp,"sdk.nupkg");
    log("Descargando runtime desde Microsoft/NuGet...");ServicePointManager.SecurityProtocol=SecurityProtocolType.Tls12;
    using(var web=new WebClient()){web.Headers[HttpRequestHeader.UserAgent]="OptiScaler-NR-GuidedSetup";await web.DownloadFileTaskAsync(new Uri("https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.721.3-preview/microsoft.direct3d.d3d12.1.721.3-preview.nupkg"),zip);}
    string license=Engine.ExtractOfficialRuntime(zip,temp,runtime);
    using(var form=new Form{Text="Licencia Microsoft 721.3-preview",Width=850,Height=620,StartPosition=FormStartPosition.CenterParent}){
     var text=new TextBox{Multiline=true,ReadOnly=true,ScrollBars=ScrollBars.Vertical,Dock=DockStyle.Fill,Text=license};
     var accept=new Button{Text="Acepto e instalar",Dock=DockStyle.Bottom,Height=40,DialogResult=DialogResult.OK};var cancel=new Button{Text="Cancelar",Dock=DockStyle.Bottom,Height=36,DialogResult=DialogResult.Cancel};form.Controls.Add(text);form.Controls.Add(cancel);form.Controls.Add(accept);
     if(form.ShowDialog(owner)!=DialogResult.OK){log("Licencia no aceptada; no se instalo el runtime.");return;}
    }
    runtimeRoot=temp;
   }
   log("Importando y registrando el modelo local; espera a que termine...");
   await Task.Run(()=>Engine.ImportLocalModel(exe,source,ready?game:runtimeRoot,log));
   log("Preparacion completada. Ejecuta Launch-OptiScaler-NR.bat y activa NR en Insert > Custom. Comprueba el temporal dentro del juego; esta prueba no lo certifica.");
  }finally{if(temp!=null&&Directory.Exists(temp)){string full=Path.GetFullPath(temp),allowed=Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar;if(full.StartsWith(allowed,StringComparison.OrdinalIgnoreCase)&&Path.GetFileName(full).StartsWith("nr-sdk-",StringComparison.Ordinal))Directory.Delete(full,true);}}
 }
}

public static partial class Engine {
 public static void CheckModelSource(string source){ValidateModelRoot(Path.GetFullPath(source));}
 public static bool FileMatches(string root,Item item){string p=Safe(root,item.path);return File.Exists(p)&&new FileInfo(p).Length==item.bytes&&Hash(p)==item.sha256;}
 public static List<Item> RuntimeContract(string executable){string game=Path.GetDirectoryName(Path.GetFullPath(executable));string path=Safe(game,"nr-launcher/runtime-contract.json");
  var state=Json.Deserialize<State>(File.ReadAllText(Journal(game)));var record=state.records.LastOrDefault(x=>x.path=="nr-launcher/runtime-contract.json");
  if(record==null||Hash(path)!=record.installedHash)throw new IOException("El contrato del runtime fue modificado.");
  var items=Json.Deserialize<List<Item>>(File.ReadAllText(path));if(items==null||items.Count==0||items.Count>32)throw new IOException("Contrato SDK invalido.");
  foreach(var item in items)if(!item.path.StartsWith("native_nr_runtime/",StringComparison.Ordinal)||item.bytes<1||item.bytes>64000000)throw new IOException("Archivo SDK no permitido.");return items;
 }
 public static string ExtractOfficialRuntime(string archive,string destination,List<Item> items){using(var zip=ZipFile.OpenRead(archive)){
  var license=zip.GetEntry("LICENSE.txt");if(license==null||license.Length>1000000)throw new IOException("Licencia SDK ausente o excesiva.");string text;using(var reader=new StreamReader(license.Open()))text=reader.ReadToEnd();
  foreach(var item in items){string name="build/native/bin/x64/"+Path.GetFileName(item.path);var entry=zip.GetEntry(name);if(entry==null||entry.Length!=item.bytes)throw new IOException("SDK oficial incompatible: "+name);string target=Safe(destination,item.path);Directory.CreateDirectory(Path.GetDirectoryName(target));entry.ExtractToFile(target);if(!FileMatches(destination,item))throw new IOException("El hash del SDK descargado no coincide: "+name);}
  return text;
 }}
 public static void ImportLocalModel(string executable,string source,string runtimeSource,Action<string> log){
  string exe=Path.GetFullPath(executable),game=Path.GetDirectoryName(exe);Closed(exe);Hardware();ValidateExecutable(exe);source=Path.GetFullPath(source);
  var state=Json.Deserialize<State>(File.ReadAllText(Journal(game)));if(state.game!=game||state.executable!=exe||!state.complete||state.removed)throw new IOException("Registro del juego invalido.");
  var manifests=ValidateModelRoot(source);var tasks=new List<Tuple<string,string>>();
  foreach(var m in manifests){string relative=(string)m["path"],file=Safe(source,relative),folder=relative.Substring(0,relative.LastIndexOf('/')+1);tasks.Add(Tuple.Create(relative,file));foreach(string row in File.ReadAllLines(file).Skip(3)){var col=row.Split(new[]{' '},3);tasks.Add(Tuple.Create(folder+col[2],Safe(source,folder+col[2])));}}
  foreach(var item in RuntimeContract(exe)){if(!FileMatches(runtimeSource,item))throw new IOException("Runtime incompleto o incompatible.");tasks.Add(Tuple.Create(item.path,Safe(runtimeSource,item.path)));}
  if(tasks.Select(x=>x.Item1).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=tasks.Count)throw new IOException("Rutas duplicadas en el paquete local.");
  var changes=tasks.Where(x=>!File.Exists(Safe(game,x.Item1))||Hash(Safe(game,x.Item1))!=Hash(x.Item2)).ToList();
  long required=changes.GroupBy(x=>Hash(x.Item2)).Sum(g=>new FileInfo(g.First().Item2).Length);
  long backupBytes=changes.Where(x=>File.Exists(Safe(game,x.Item1))).Sum(x=>new FileInfo(Safe(game,x.Item1)).Length);
  if(new DriveInfo(Path.GetPathRoot(game)).AvailableFreeSpace<required+backupBytes+128000000)throw new IOException("Espacio insuficiente para importar y respaldar el modelo.");
  string offIni=File.ReadAllText(Safe(game,"native_nr.ini"));offIni=System.Text.RegularExpressions.Regex.Replace(offIni,@"(?m)^GlobalB1\s*=.*$","GlobalB1=0");offIni=System.Text.RegularExpressions.Regex.Replace(offIni,@"(?m)^Enabled\s*=.*$","Enabled=0");WriteManaged(state,"native_nr.ini",System.Text.Encoding.UTF8.GetBytes(offIni),log);
  var offProfile=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Safe(game,"nr-launcher/profile.json")));offProfile["model_required"]=true;WriteManaged(state,"nr-launcher/profile.json",System.Text.Encoding.UTF8.GetBytes(Json.Serialize(offProfile)),log);
  var known=new Dictionary<string,string>();int count=0;
  foreach(var task in changes){Closed(exe);string target=Safe(game,task.Item1),digest=Hash(task.Item2);if(File.Exists(target)&&Hash(target)==digest){known[digest]=target;continue;}
   string old=File.Exists(target)?Hash(target):null;var record=new Record{path=task.Item1,installedHash=digest,previousHash=old,managed=true};
   if(old!=null){record.backup=state.backupDirectory+"/import/"+state.records.Count+"/"+task.Item1;string b=Safe(game,record.backup);Directory.CreateDirectory(Path.GetDirectoryName(b));File.Copy(target,b,false);if(Hash(b)!=old)throw new IOException("Respaldo de importacion incorrecto.");}
   state.records.Add(record);WriteJson(Journal(game),state);Directory.CreateDirectory(Path.GetDirectoryName(target));string temp=target+".nr-install-new";if(File.Exists(temp))throw new IOException("Archivo temporal previo.");
   try{string prior;bool linked=known.TryGetValue(digest,out prior)&&CreateHardLink(temp,prior,IntPtr.Zero);if(!linked)File.Copy(task.Item2,temp,false);if(Hash(temp)!=digest)throw new IOException("Copia del modelo incorrecta.");Atomic(temp,target);known[digest]=target;}finally{if(File.Exists(temp))File.Delete(temp);}
   if(++count%100==0)log("Archivos importados: "+count+" / "+tasks.Count);
  }
  RegisterLocalModel(exe,log);
 }
}
