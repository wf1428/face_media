var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);
var stdin_exports = {};
__export(stdin_exports, {
  default: () => stdin_default
});
module.exports = __toCommonJS(stdin_exports);
var stdin_default = {
  name: "Namn",
  tel: "Telefon",
  save: "Lagre",
  clear: "T\xF8m",
  undo: "Angre",
  cancel: "Avbryt",
  confirm: "Stadfest",
  delete: "Slett",
  loading: "Lastar...",
  noCoupon: "Ingen kupongar",
  nameEmpty: "Fyll inn namn",
  addContact: "Legg til ny kontakt",
  telInvalid: "Ugyldig telefonnummer",
  vanCalendar: {
    end: "Slutt",
    start: "Start",
    title: "Kalender",
    weekdays: ["Sun", "M\xE5n", "Tys", "Ons", "Tor", "Fre", "Lau"],
    monthTitle: (year, month) => `${year}/${month}`,
    rangePrompt: (maxRange) => `Maks. ${maxRange} dagar`
  },
  vanCascader: {
    select: "Vel"
  },
  vanPagination: {
    prev: "F\xF8rre",
    next: "Neste"
  },
  vanPullRefresh: {
    pulling: "Dra for \xE5 oppdatere...",
    loosing: "Slepp for \xE5 oppdatere..."
  },
  vanSubmitBar: {
    label: "Totalt:"
  },
  vanCoupon: {
    unlimited: "Ubegrensa",
    discount: (discount) => `${discount * 10}% avslag`,
    condition: (condition) => `Minst ${condition}`
  },
  vanCouponCell: {
    title: "Kupong",
    count: (count) => `Du har ${count} kupongar`
  },
  vanCouponList: {
    exchange: "Byt",
    close: "Lukk",
    enable: "Tilgjengeleg",
    disabled: "Utilgjengeleg",
    placeholder: "Kupongkode"
  },
  vanAddressEdit: {
    area: "Omr\xE5de",
    areaEmpty: "Vel omr\xE5de",
    addressEmpty: "Adressa kan ikkje vere tom",
    addressDetail: "Adresse",
    defaultAddress: "Set som standardadresse"
  },
  vanAddressList: {
    add: "Legg til ny adresse"
  }
};
