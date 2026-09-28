// Builds the list from window.__uutiset once the document is ready.
document.addEventListener("DOMContentLoaded", function () {
  var lista = document.getElementById("lista");
  window.__uutiset.forEach(function (u) {
    var card = document.createElement("div");
    card.className = "card";
    card.innerHTML = '<div class="card__body"><span class="tag"></span><h3 class="card__title"></h3><p class="card__text"></p></div>';
    card.querySelector(".tag").textContent = u.aihe;
    card.querySelector(".card__title").textContent = u.otsikko;
    card.querySelector(".card__text").textContent = "Julkaistu klo " + u.aika;
    lista.appendChild(card);
  });
  var tila = document.getElementById("tila");
  tila.textContent = "Skripti rakensi " + lista.querySelectorAll(".card").length + " korttia.";
  setTimeout(function () { tila.textContent += " Ajastin toimii myös."; }, 500);
  fetch("https://example.com/").catch(function () { tila.textContent += " Verkko on estetty."; });
});
