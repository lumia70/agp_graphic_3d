import lumeapp from 'liblumeapp.so';

@Entry
@Component
struct Index {
  private xComponentId: string = 'lumeapp_xcomponent';
  private intervalId: number = -1;
  // Order must match kDemoModels[] in render_main.cpp.
  @State modelIndex: number = 4;
  @State firstPerson: boolean = true;
  @State budgetIndex: number = 1;

  private static readonly MODELS: string[] = [
    'fruit_bowl.ply', 'fruit_bowl.sog', 'godzilla.splat', 'doll.glb', 'syp_uav_0611'
  ];
  private static readonly MODEL_LABELS: string[] = [
    'Model: Bowl PLY', 'Model: Bowl SOG', 'Model: Godzilla', 'Model: Doll', 'Model: City LOD'
  ];
  private static readonly BUDGETS: number[] = [1500000, 900000, 600000];
  private static readonly BUDGET_LABELS: string[] = [
    'Splats: 1.5M (sharp)', 'Splats: 900k (balanced)', 'Splats: 600k (fast)'
  ];

  aboutToAppear(): void {
    let context = getContext(this) as Context;
    let resMgr = context.resourceManager;
    let filesDir = context.filesDir;
    lumeapp.initResourceManager(resMgr, filesDir);
    console.info('[LumeApp] ResourceManager + filesDir passed to native: ' + filesDir);
  }

  private selectModel(index: number): void {
    if (index === this.modelIndex) {
      return;
    }
    this.modelIndex = index;
    const name = Index.MODELS[index];
    lumeapp.setModel(name);
    // Large LOD scenes default to first-person roaming, single models orbit.
    this.firstPerson = name === 'syp_uav_0611';
    lumeapp.setCameraMode(this.firstPerson);
    console.info('[LumeApp] switch model to ' + name + ', firstPerson=' + this.firstPerson);
  }

  private selectCameraMode(index: number): void {
    const firstPerson = index === 0;
    if (firstPerson === this.firstPerson) {
      return;
    }
    this.firstPerson = firstPerson;
    lumeapp.setCameraMode(firstPerson);
    console.info('[LumeApp] camera mode firstPerson=' + firstPerson);
  }

  private selectBudget(index: number): void {
    this.budgetIndex = index;
    lumeapp.setLodBudget(Index.BUDGETS[index]);
    console.info('[LumeApp] lod budget ' + Index.BUDGETS[index]);
  }

  private static readonly MODEL_OPTIONS: SelectOption[] = [
    { value: 'Model: Bowl PLY' }, { value: 'Model: Bowl SOG' }, { value: 'Model: Godzilla' },
    { value: 'Model: Doll' }, { value: 'Model: City LOD' }
  ];
  private static readonly CAMERA_OPTIONS: SelectOption[] = [
    { value: 'Camera: First-person' }, { value: 'Camera: Orbit' }
  ];
  private static readonly BUDGET_OPTIONS: SelectOption[] = [
    { value: 'Splats: 1.5M (sharp)' }, { value: 'Splats: 900k (balanced)' },
    { value: 'Splats: 600k (fast)' }
  ];

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

      Column({ space: 6 }) {
        Select(Index.MODEL_OPTIONS)
          .selected(this.modelIndex)
          .value(Index.MODEL_LABELS[this.modelIndex])
          .font({ size: 12 })
          .optionFont({ size: 12 })
          .height(32)
          .width(180)
          .backgroundColor('#99000000')
          .onSelect((index: number) => this.selectModel(index))

        Select(Index.CAMERA_OPTIONS)
          .selected(this.firstPerson ? 0 : 1)
          .value(this.firstPerson ? 'Camera: First-person' : 'Camera: Orbit')
          .font({ size: 12 })
          .optionFont({ size: 12 })
          .height(32)
          .width(180)
          .backgroundColor('#99000000')
          .onSelect((index: number) => this.selectCameraMode(index))

        Select(Index.BUDGET_OPTIONS)
          .selected(this.budgetIndex)
          .value(Index.BUDGET_LABELS[this.budgetIndex])
          .font({ size: 12 })
          .optionFont({ size: 12 })
          .height(32)
          .width(180)
          .backgroundColor('#99000000')
          .onSelect((index: number) => this.selectBudget(index))
      }
      .alignItems(HorizontalAlign.End)
      .margin({ bottom: 28, right: 12 })
    }
    .width('100%')
    .height('100%')
  }
}
