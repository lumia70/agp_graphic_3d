import lumeapp from 'liblumeapp.so';

@Entry
@Component
struct Index {
  private xComponentId: string = 'lumeapp_xcomponent';
  private intervalId: number = -1;
  @State activeModel: string = 'doll.glb';
  @State firstPerson: boolean = false;

  private static readonly MODELS: string[] = ['fruit_bowl.ply', 'fruit_bowl.sog', 'godzilla.splat', 'doll.glb'];

  aboutToAppear(): void {
    let context = getContext(this) as Context;
    let resMgr = context.resourceManager;
    let filesDir = context.filesDir;
    lumeapp.initResourceManager(resMgr, filesDir);
    console.info('[LumeApp] ResourceManager + filesDir passed to native: ' + filesDir);
  }

  private selectModel(name: string): void {
    if (this.activeModel === name) {
      return;
    }
    this.activeModel = name;
    lumeapp.setModel(name);
    // Large LOD scenes default to first-person roaming, single models orbit.
    this.firstPerson = name === 'syp_uav_0611';
    lumeapp.setCameraMode(this.firstPerson);
    console.info('[LumeApp] switch model to ' + name + ', firstPerson=' + this.firstPerson);
  }

  private toggleMode(): void {
    this.firstPerson = !this.firstPerson;
    lumeapp.setCameraMode(this.firstPerson);
    console.info('[LumeApp] camera mode firstPerson=' + this.firstPerson);
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      XComponent({
        id: this.xComponentId,
        type: XComponentType.SURFACE,
        libraryname: 'lumeapp'
      })
        .onLoad(() => {
          console.info('[LumeApp] XComponent onLoad - starting render loop');
          this.intervalId = setInterval(() => {
            lumeapp.drawFrame();
          }, 16);
        })
        .onDestroy(() => {
          console.info('[LumeApp] XComponent onDestroy');
          if (this.intervalId >= 0) {
            clearInterval(this.intervalId);
            this.intervalId = -1;
          }
        })
        .width('100%')
        .height('100%')

      Column({ space: 8 }) {
        Button(this.firstPerson ? 'Mode: First-person' : 'Mode: Orbit')
          .fontSize(12)
          .height(30)
          .width(170)
          .backgroundColor(this.firstPerson ? '#CC1E88E5' : '#66000000')
          .onClick(() => this.toggleMode())

        Row({ space: 6 }) {
        Button('Bowl PLY')
          .fontSize(11)
          .height(32)
          .width(72)
          .backgroundColor(this.activeModel === 'fruit_bowl.ply' ? '#FF6A00' : '#66000000')
          .onClick(() => this.selectModel('fruit_bowl.ply'))
        Button('Bowl SOG')
          .fontSize(11)
          .height(32)
          .width(72)
          .backgroundColor(this.activeModel === 'fruit_bowl.sog' ? '#FF6A00' : '#66000000')
          .onClick(() => this.selectModel('fruit_bowl.sog'))
        Button('Godzilla')
          .fontSize(11)
          .height(32)
          .width(72)
          .backgroundColor(this.activeModel === 'godzilla.splat' ? '#FF6A00' : '#66000000')
          .onClick(() => this.selectModel('godzilla.splat'))
        Button('Doll')
          .fontSize(11)
          .height(32)
          .width(72)
          .backgroundColor(this.activeModel === 'doll.glb' ? '#FF6A00' : '#66000000')
          .onClick(() => this.selectModel('doll.glb'))
        Button('City LOD')
          .fontSize(11)
          .height(32)
          .width(72)
          .backgroundColor(this.activeModel === 'syp_uav_0611' ? '#FF6A00' : '#66000000')
          .onClick(() => this.selectModel('syp_uav_0611'))
        }
      }
      .margin({ bottom: 28 })
    }
    .width('100%')
    .height('100%')
  }
}
