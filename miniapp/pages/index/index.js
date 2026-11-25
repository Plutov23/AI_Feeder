const config = require('../../config')

const POLLING_INTERVAL_MS = 5000
const ANALYSIS_TIMEOUT_MS = 60000

Page({
  data: {
    activeTab: 'data',
    foodTrend: '待接入',
    dailyEatingCount: '--',
    lastEatingTime: '--:--',
    dataReady: false,
    aiAnalysis: '正在读取最新分析结果…',
    isAnalyzing: false,
    deviceOnline: false,
    statusText: '正在连接',
    lastUpdated: '--:--'
  },

  onLoad() {
    this.analysisBaseline = ''
    this.commandAcknowledged = false
    this.fetchCloudData()
    this.pollingTimer = setInterval(
      () => this.fetchCloudData(),
      POLLING_INTERVAL_MS
    )
  },

  onUnload() {
    if (this.pollingTimer) clearInterval(this.pollingTimer)
    if (this.analysisTimeout) clearTimeout(this.analysisTimeout)
  },

  formatTime(date) {
    const hours = date.getHours().toString().padStart(2, '0')
    const minutes = date.getMinutes().toString().padStart(2, '0')
    return hours + ':' + minutes
  },

  switchTab(event) {
    const tab = event.currentTarget.dataset.tab
    if (tab !== 'data' && tab !== 'ai') return
    this.setData({ activeTab: tab })
  },

  findProperty(properties, identifier) {
    return properties.find(item => item.identifier === identifier)
  },

  isCommandActive(value) {
    return value === true || value === 1 ||
      value === '1' || value === 'true'
  },

  finishAnalysis(update) {
    if (this.analysisTimeout) {
      clearTimeout(this.analysisTimeout)
      this.analysisTimeout = null
    }
    update.isAnalyzing = false
    update.statusText = '设备在线'
    this.commandAcknowledged = false
  },

  fetchCloudData() {
    const url = config.onenet.apiBaseUrl +
      '/thingmodel/query-device-property?product_id=' +
      config.onenet.productId +
      '&device_name=' + config.onenet.deviceName

    wx.request({
      url,
      method: 'GET',
      header: {
        Authorization: config.onenet.authorization
      },
      success: res => {
        if (!res.data || res.data.code !== 0 ||
            !Array.isArray(res.data.data)) {
          this.setData({
            deviceOnline: false,
            statusText: '数据连接异常'
          })
          return
        }

        const properties = res.data.data
        const replyProperty = this.findProperty(properties, 'ai_reply')
        const commandProperty = this.findProperty(properties, 'ai_cmd')
        const propertyIds = config.onenet.properties
        const foodTrendProperty = this.findProperty(
          properties,
          propertyIds.foodTrend
        )
        const dailyCountProperty = this.findProperty(
          properties,
          propertyIds.dailyEatingCount
        )
        const lastEatingProperty = this.findProperty(
          properties,
          propertyIds.lastEatingTime
        )
        const reply = replyProperty?.value || ''
        const commandActive = this.isCommandActive(commandProperty?.value)
        const update = {
          deviceOnline: true,
          lastUpdated: this.formatTime(new Date())
        }

        const hasFoodTrend = foodTrendProperty?.value !== undefined
        const hasDailyCount = dailyCountProperty?.value !== undefined
        const hasLastEating = lastEatingProperty?.value !== undefined

        if (hasFoodTrend) {
          update.foodTrend = String(foodTrendProperty.value)
        }
        if (hasDailyCount) {
          update.dailyEatingCount = String(dailyCountProperty.value)
        }
        if (hasLastEating) {
          update.lastEatingTime = String(lastEatingProperty.value)
        }
        update.dataReady = hasFoodTrend && hasDailyCount && hasLastEating

        if (reply) update.aiAnalysis = reply

        if (this.data.isAnalyzing) {
          const processing = commandActive || reply === '分析中...'
          if (processing) {
            this.commandAcknowledged = true
            update.statusText = '分析处理中'
          } else if (reply && (
            this.commandAcknowledged ||
            reply !== this.analysisBaseline
          )) {
            this.finishAnalysis(update)
          }
        } else {
          update.statusText = '设备在线'
        }

        this.setData(update)
      },
      fail: () => {
        this.setData({
          deviceOnline: false,
          statusText: '无法连接 OneNET'
        })
      }
    })
  },

  analyzeWithDeepSeek() {
    if (this.data.isAnalyzing) return

    this.analysisBaseline = this.data.aiAnalysis
    this.commandAcknowledged = false
    this.setData({
      isAnalyzing: true,
      statusText: '正在发送指令'
    })

    this.analysisTimeout = setTimeout(() => {
      this.analysisTimeout = null
      this.setData({
        isAnalyzing: false,
        statusText: '分析请求超时'
      })
    }, ANALYSIS_TIMEOUT_MS)

    wx.request({
      url: config.onenet.apiBaseUrl +
        '/thingmodel/set-device-property',
      method: 'POST',
      header: {
        Authorization: config.onenet.authorization,
        'Content-Type': 'application/json'
      },
      data: {
        product_id: config.onenet.productId,
        device_name: config.onenet.deviceName,
        params: {
          ai_cmd: true
        }
      },
      success: res => {
        if (res.data && res.data.code === 0) {
          this.setData({ statusText: '等待 AI 返回' })
          return
        }

        if (this.analysisTimeout) clearTimeout(this.analysisTimeout)
        this.analysisTimeout = null
        this.setData({
          isAnalyzing: false,
          statusText: '指令发送失败'
        })
      },
      fail: () => {
        if (this.analysisTimeout) clearTimeout(this.analysisTimeout)
        this.analysisTimeout = null
        this.setData({
          isAnalyzing: false,
          statusText: '无法连接 OneNET'
        })
      }
    })
  }
})
