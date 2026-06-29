//ref: http://github.com/microsoft/Windows-driver-samples/blob/main/input/kbfiltr/sys/kbfiltr.c
//https://learn.microsoft.com/es-es/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers
//https://learn.microsoft.com/es-es/windows-hardware/drivers/ddi/kbdmou/ni-kbdmou-ioctl_internal_keyboard_connect
//https://learn.microsoft.com/es-es/windows-hardware/drivers/ddi/kbdmou/ni-kbdmou-ioctl_internal_mouse_connect




#include <ntddk.h>
// Cabecera principal del WDK para drivers kernel mode.
// Define NTSTATUS, PDRIVER_OBJECT, PIRP, DbgPrint, IoCreateDevice
// Es la que posee las funciiones basicas del kernel

#include <kbdmou.h>
// Cabecera específica de teclado y ratón del WDK.
// Define PKEYBOARD_INPUT_DATA, KEY_BREAK, KEY_MAKE.




NTSTATUS DispatchRead(PDEVICE_OBJECT DevObj, PIRP Irp);
// Firma de la función que intercepta IRPs de lectura.
// NTSTATUS = código de retorno del kernel (STATUS_SUCCESS, etc.)
// PDEVICE_OBJECT = puntero a nuestro dispositivo filtro.
// PIRP = puntero al paquete de E/S (la "señal" del autómata).

NTSTATUS ReadComplete(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context);
// Firma de la completion routine.
// Se llama cuando kbdclass termina de leer la tecla.
// PVOID Context = puntero opcional que pasamos nosotros (aquí NULL).

VOID Unload(PDRIVER_OBJECT DriverObject);
// El kernel la llama cuando haces "sc stop".


// ─── VAR GLOBALES ─────────────────────────────────────────────────────────────────

// Puntero a nuestro dispositivo filtro 
// Es el objeto que el kernel mete en la pila encima de kbdclass.
// Empieza en NULL, se rellena en IoCreateDevice.
PDEVICE_OBJECT gFilterDevice = NULL;


// Puntero al dispositivo que está debajo de nosotros en la pila (kbdclass).
// Lo necesitamos para pasarle los IRPs con IoCallDriver.
// Empieza en NULL, se rellena en IoAttachDevice.
PDEVICE_OBJECT gNextDevice = NULL;



// Punto de entrada del driver. Equivale al constructor.
// El kernel lo llama UNA sola vez al cargar el .sys.
// pDriverObj = objeto que representa a este driver (la tabla de callbacks vive aquí).
// pRegistryPath = ruta en el registro donde están los parámetros del driver.
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObj, PUNICODE_STRING pRegistryPath) {

    // Le decimos al compilador que no usamos pRegistryPath.
    // Evita warnings absurdos
    UNREFERENCED_PARAMETER(pRegistryPath);
 
    // Variable local para guardar el resultado de cada llamada al kernel.
    // Patrón estándar en drivers: cada función devuelve un NTSTATUS.
    NTSTATUS status;
    
    // Crea un objeto DEVICE_OBJECT en el kernel y lo asigna a gFilterDevice.
    // sirve para meternos en la pila de dispositivos..
    status = IoCreateDevice(
      
        // A qué driver pertenece este dispositivo (el nuestro).
        pDriverObj,
       
        // Tamaño extra de memoria privada por dispositivo (device extension).
        // 0 = no necesitamos datos extra por ahora.
        0,
     
        // Nombre del dispositivo en el namespace del kernel (ej: \Device\MiFiltro).
        // No es necesario actualmente.
        NULL,
      
        // Tipo de dispositivo. Le dice al kernel que somos un teclado.
        // Importante para que la pila sea compatible con kbdclass.
        FILE_DEVICE_KEYBOARD,
     
        // Características extra del dispositivo
        0,
       
        // quitamos la exclusividad para que todos puedan consultarlo
        FALSE,
    
        // Aquí se referencia el filter que hemos creado. 
        &gFilterDevice
       
    );

    // NT_SUCCESS comprueba si el NTSTATUS indica éxito (bit alto = 0).

    if (!NT_SUCCESS(status)) {
      
        // Imprime el código de error en DebugView para saber qué pasó.
        DbgPrint("IoCreateDevice fallo: 0x%X\n", status);
      
        // Devolvemos el error al kernel. El driver no se carga.
        return status;
        
    }
    // Estructura del kernel para strings Unicode.
   // El kernel no usa char* ni std::string, usa UNICODE_STRING.
    UNICODE_STRING kbdName;
   
    // Inicializa kbdName con el nombre del dispositivo de kbdclass.
    // KeyboardClass0 = primer teclado del sistema.
    RtlInitUnicodeString(&kbdName, L"\\Device\\KeyboardClass0");
    
    // Mete gFilterDevice encima de KeyboardClass0 en la pila.
    // A partir de aquí los IRPs de teclado nos llegan a nosotros primero.
    // gNextDevice se rellena con el puntero a kbdclass (el que estaba antes arriba).
    status = IoAttachDevice(gFilterDevice, &kbdName, &gNextDevice);


    if (!NT_SUCCESS(status)) {
        DbgPrint("IoAttachDevice fallo: 0x%X\n", status);
        // Si el attach falla hay que limpiar el dispositivo que creamos antes.
       // Si no, dejamos un objeto huérfano en el kernel — memory leak.
        IoDeleteDevice(gFilterDevice);
       

        return status;
    }
    // Copiamos el tipo de dispositivo de kbdclass al nuestro.
     // La pila asume que todos los dispositivos son del mismo tipo.
     // Si no lo copiamos, el kernel puede rechazar nuestros IRPs.
    gFilterDevice->DeviceType = gNextDevice->DeviceType;
 
    // Copiamos las características (ej: si es removible, si es de solo lectura). Por consistencia
    gFilterDevice->Characteristics = gNextDevice->Characteristics;
 
      // La stack size indica cuántos niveles de IRP stack location se necesitan. Solo vamos a añadir uno más
      // Si no lo ajustamos, nos quedamos sin stack locations al pasar IRPs.
    gFilterDevice->StackSize = gNextDevice->StackSize + 1;
  
    // Copiamos el modo de E/S de kbdclass (buffered o direct).
     // Determina cómo el kernel mapea los buffers de datos en memoria.
     // Si no coincide con el de abajo, los punteros al buffer son incorrectos
     // y lees basura o crasheas.
    gFilterDevice->Flags |= gNextDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);
 
    // Quita el flag que dice "este dispositivo aún se está inicializando".
   // El kernel lo pone automáticamente en IoCreateDevice.
   // Si no lo quitamos, el kernel rechaza los IRPs — el filtro no funciona.
    gFilterDevice->Flags &= ~DO_DEVICE_INITIALIZING;
   
    // Registra DispatchRead como el callback para IRPs de lectura.
    //Sirve para que el kernele ejecute la función esperada
    pDriverObj->MajorFunction[IRP_MJ_READ] = DispatchRead;

    // Declarar la finalización del driver.
    pDriverObj->DriverUnload = Unload;
  

    DbgPrint("Filtro cargado y adjuntado a KeyboardClass0\n");
   
    //Confirmación de que no hubo problemas
    return STATUS_SUCCESS;
 
}

// ─── UTILIDAD ────────────────────────────────────────────────────────────
  // El kernel llama a esta función cada vez que llega un IRP_MJ_READ.
    // En un teclado eso ocurre cada vez que se pulsa o suelta una tecla.
    // En este momento el buffer del IRP está vacío — kbdclass aún no leyó nada.
NTSTATUS DispatchRead(PDEVICE_OBJECT DevObj, PIRP Irp) {
  
    //para no generar warning no deseados
    UNREFERENCED_PARAMETER(DevObj);
 
    // Copia el stack location actual al siguiente nivel de la pila.
     // Cada driver en la pila tiene su propio "slot" en el IRP.
     // Sin esto kbdclass no sabe qué operación tiene que hacer.
    IoCopyCurrentIrpStackLocationToNext(Irp);
 
     // Registra ReadComplete para que el kernel la llame
        // cuando kbdclass termine de procesar este IRP.
    IoSetCompletionRoutine(
       
        // El IRP al que adjuntamos la completion routine.
        Irp,
       
        // Nuestra función de callback post-procesado.
        ReadComplete,
        
        // Contexto opcional que se pasa a ReadComplete.
       // NULL = no necesitamos pasar datos extra.
        NULL,
       
        // Llamar a ReadComplete si el IRP tuvo éxito.
        TRUE,
      
        // Llamar a ReadComplete si el IRP tuvo error.
        TRUE,
        
        // Llamar a ReadComplete si el IRP fue cancelado.
       // Los tres TRUE = llamar siempre, pase lo que pase.
        TRUE
       
    );
    // Pasa el IRP hacia abajo a kbdclass.
   // gNextDevice = puntero a kbdclass que guardamos al hacer IoAttachDevice.
   // A partir de aquí kbdclass hace la lectura real del hardware.
   // Cuando termine, el kernel llamará automáticamente a ReadComplete.
    return IoCallDriver(gNextDevice, Irp);
   
}

// ───  ROUTINE ───────────────────────────────────────────────────────
    // El kernel llama a esta función cuando kbdclass terminó de leer la tecla.
    // Ahora sí el buffer del IRP tiene datos reales.
NTSTATUS ReadComplete(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context) {


    UNREFERENCED_PARAMETER(DevObj);
    UNREFERENCED_PARAMETER(Context);

    // Si kbdclass devolvió STATUS_PENDING (operación asíncrona),
   // debemos marcarlo también en nuestro stack location.
   // Si no lo hacemos, el kernel detecta inconsistencia y crashea.
   // Es una regla obligatoria: si PendingReturned está activo, debes marcarlo.
    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
   
    // Irp->IoStatus.Status = resultado de kbdclass (¿leyó bien?).
       // Irp->IoStatus.Information = bytes escritos en el buffer.
       // Si es 0, el buffer está vacío — no hay tecla que leer.
       // Este guard evita leer memoria no inicializada y crashear.
    if (NT_SUCCESS(Irp->IoStatus.Status) && Irp->IoStatus.Information > 0) {
       
        // SystemBuffer apunta al buffer donde kbdclass escribió los datos.
   // Lo casteamos a KEYBOARD_INPUT_DATA, la estructura que define una tecla.
   // Campos principales:
   //   MakeCode = código de la tecla (scancode).
   //   Flags    = KEY_MAKE (pulsada) o KEY_BREAK (soltada).
        PKEYBOARD_INPUT_DATA keys =
            (PKEYBOARD_INPUT_DATA)Irp->AssociatedIrp.SystemBuffer;
   
        // Scancode de la tecla en hexadecimal. Ej: 0x1E = la 'A'.
        DbgPrint("Tecla: 0x%X %s\n",
            keys->MakeCode,
            
            // KEY_BREAK = dedo levantado. KEY_MAKE = dedo bajando.
            keys->Flags == KEY_BREAK ? "soltada" : "pulsada"
           
        );
    }
    // Le dice al kernel "sigue procesando este IRP hacia arriba".
 // Si devolvieras STATUS_MORE_PROCESSING_REQUIRED detendrías el IRP aquí
 // y tendrías que completarlo tú manualmente más tarde.
 // STATUS_CONTINUE_COMPLETION = comportamiento normal, déjalo subir.
    return STATUS_CONTINUE_COMPLETION;
 
}

// ─── UNLOAD ───────────────────────────────────────────────────────────────────
 // El kernel llama a esto cuando haces "sc stop NombreDriver".
    // Debes limpiar todo lo que creaste en DriverEntry, en orden inverso.
VOID Unload(PDRIVER_OBJECT DriverObject) {
   

    UNREFERENCED_PARAMETER(DriverObject);
    // Nos sacamos de la pila de dispositivos.
   // A partir de aquí los IRPs de teclado ya no nos llegan.
   // Comprobamos que no sea NULL por si el attach falló parcialmente.
    if (gNextDevice)
        IoDetachDevice(gNextDevice);
   
    // Destruye el objeto DEVICE_OBJECT que creamos en IoCreateDevice.
      // Libera la memoria del kernel asociada a él.
      // Siempre después de IoDetachDevice — nunca antes.
    if (gFilterDevice)
        IoDeleteDevice(gFilterDevice);
  

    DbgPrint("Driver descargado\n");
}